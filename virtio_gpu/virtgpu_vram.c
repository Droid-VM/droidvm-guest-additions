// SPDX-License-Identifier: GPL-2.0
#include "virtgpu_drv.h"

#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sizes.h>

static void virtio_gpu_vram_free(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);
	bool unmap;

	if (bo->created) {
		spin_lock(&vgdev->host_visible_lock);
		unmap = drm_mm_node_allocated(&vram->vram_node);
		spin_unlock(&vgdev->host_visible_lock);

		/*
		 * Gunyah acceptance (and its release-before-unmap ordering, which keeps the
		 * host from re-SHAREing a reused BAR offset while the guest still maps it)
		 * is driven host-side over the virtio-gunyah-accept transport, inside the
		 * unmap below. Nothing to do here.
		 */
		if (unmap)
			virtio_gpu_cmd_unmap(vgdev, bo);

		virtio_gpu_cmd_unref_resource(vgdev, bo);
		virtio_gpu_notify(vgdev);

		/*
		 * DroidVM guest-alloc: this blob's backing was sub-allocated by us from the
		 * guest-alloc pool. The host has now dropped its reference (unref above), so
		 * return the pages to our bitmap. (Host-alloc pool_resident blobs are freed
		 * host-side and never set guest_pool_owned.)
		 */
		if (vram->guest_pool_owned)
			virtio_gpu_guest_pool_free(vgdev, vram->pool_offset,
						   PFN_UP(obj->size));
		return;
	}
}

static const struct vm_operations_struct virtio_gpu_vram_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int virtio_gpu_vram_mmap(struct drm_gem_object *obj,
				struct vm_area_struct *vma)
{
	int ret;
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);
	unsigned long vm_size = vma->vm_end - vma->vm_start;
	unsigned long vm_end;

	/* Guest-alloc blobs are always guest-mappable (the guest owns the backing), even when
	 * the guest ICD did not set USE_MAPPABLE on the create. */
	if (!(bo->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE) && !vram->guest_pool_owned)
		return -EINVAL;

	/* Guest-alloc blobs never wait on a host map response (STATE_OK is set at create). */
	if (!vram->guest_pool_owned) {
		wait_event(vgdev->resp_wq, vram->map_state != STATE_INITIALIZING);
		if (vram->map_state != STATE_OK)
			return -EINVAL;
	}

	/*
	 * Gunyah: the host SHARE'd this blob and drove the guest-side memparcel accept
	 * itself, over the virtio-gunyah-accept transport, before the map_blob response
	 * came back -- so the IPA at vram_node.start is already accessible here and this
	 * driver needs no memparcel code at all. (gfxstream pre-alloc blobs never even
	 * SHARE: the pool was blessed at boot.)
	 */

	vma->vm_pgoff -= drm_vma_node_start(&obj->vma_node);
	vm_flags_set(vma, VM_MIXEDMAP | VM_DONTEXPAND);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);
	vma->vm_ops = &virtio_gpu_vram_vm_ops;

	if (vram->map_info == VIRTIO_GPU_MAP_CACHE_WC)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	else if (vram->map_info == VIRTIO_GPU_MAP_CACHE_UNCACHED)
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (check_add_overflow(vma->vm_pgoff << PAGE_SHIFT, vm_size, &vm_end))
		return -EINVAL;

	if (vm_end > vram->vram_node.size)
		return -EINVAL;

	/*
	 * gfxstream pre-alloc: remap the GpuPool page (gpu_pool_base + pool_offset)
	 * instead of the BAR node. The pool is already in the guest stage-2, so no
	 * accept was needed. (3b will io_remap a run list for fragmented blobs.)
	 */
	if (vram->pool_resident) {
		phys_addr_t base, pa;

		/*
		 * DroidVM guest-alloc blobs live in the guest-alloc pool (gpu_guest_pool_base),
		 * which the guest driver owns; host-alloc pool_resident blobs live in the
		 * host pool (gpu_pool_base).
		 */
		base = vram->guest_pool_owned ? vgdev->gpu_guest_pool_base
					      : vgdev->gpu_pool_base;
		if (!base) {
			pr_err("virtio-gpu: pool-resident blob but no pool base in DT (guest=%d)\n",
			       vram->guest_pool_owned);
			return -EINVAL;
		}
		pa = base + vram->pool_offset;
		return io_remap_pfn_range(vma, vma->vm_start,
					  (pa >> PAGE_SHIFT) + vma->vm_pgoff,
					  vm_size, vma->vm_page_prot);
	}

	ret = io_remap_pfn_range(vma, vma->vm_start,
				 (vram->vram_node.start >> PAGE_SHIFT) + vma->vm_pgoff,
				 vm_size, vma->vm_page_prot);
	return ret;
}

struct sg_table *virtio_gpu_vram_map_dma_buf(struct virtio_gpu_object *bo,
					     struct device *dev,
					     enum dma_data_direction dir)
{
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;
	struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);
	struct sg_table *sgt;
	dma_addr_t addr;
	int ret;

	sgt = kzalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	if (!(bo->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE)) {
		// Virtio devices can access the dma-buf via its UUID. Return a stub
		// sg_table so the dma-buf API still works.
		if (!is_virtio_device(dev) || !vgdev->has_resource_assign_uuid) {
			ret = -EIO;
			goto out;
		}
		return sgt;
	}

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret)
		goto out;

	addr = dma_map_resource(dev, vram->vram_node.start,
				vram->vram_node.size, dir,
				DMA_ATTR_SKIP_CPU_SYNC);
	ret = dma_mapping_error(dev, addr);
	if (ret)
		goto out;

	sg_set_page(sgt->sgl, NULL, vram->vram_node.size, 0);
	sg_dma_address(sgt->sgl) = addr;
	sg_dma_len(sgt->sgl) = vram->vram_node.size;

	return sgt;
out:
	sg_free_table(sgt);
	kfree(sgt);
	return ERR_PTR(ret);
}

void virtio_gpu_vram_unmap_dma_buf(struct device *dev,
				   struct sg_table *sgt,
				   enum dma_data_direction dir)
{
	if (sgt->nents) {
		dma_unmap_resource(dev, sg_dma_address(sgt->sgl),
				   sg_dma_len(sgt->sgl), dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	}
	sg_free_table(sgt);
	kfree(sgt);
}

static const struct drm_gem_object_funcs virtio_gpu_vram_funcs = {
	.open = virtio_gpu_gem_object_open,
	.close = virtio_gpu_gem_object_close,
	.free = virtio_gpu_vram_free,
	.mmap = virtio_gpu_vram_mmap,
	.export = virtgpu_gem_prime_export,
};

bool virtio_gpu_is_vram(struct virtio_gpu_object *bo)
{
	return bo->base.base.funcs == &virtio_gpu_vram_funcs;
}

static int virtio_gpu_vram_map(struct virtio_gpu_object *bo)
{
	int ret;
	uint64_t offset;
	struct virtio_gpu_object_array *objs;
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;
	struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);

	if (!vgdev->has_host_visible)
		return -EINVAL;

	/*
	 * host_visible_mm's backing is hugepage-backed by gh_hugepage_reserve's pool: each 2MB
	 * physical folio can apparently host only ONE live gunyah SHARE/MEM_ACCEPT at a time. A
	 * plain byte-granular sub-allocator packs unrelated blobs into the same 2MB folio, and
	 * the second blob's accept then collides with the first's still-live stage-2 mapping of
	 * that folio and is rejected (MEM_ACCEPT err_code=0x6/0x7) even though it never touches
	 * the first blob's bytes. Force every blob onto its own dedicated, exclusively-owned 2MB
	 * folio(s): align the start to 2MB and round the size up to a 2MB multiple so no other
	 * blob's allocation can ever land in the same folio.
	 */
	spin_lock(&vgdev->host_visible_lock);
	ret = drm_mm_insert_node_generic(&vgdev->host_visible_mm, &vram->vram_node,
					 ALIGN(bo->base.base.size, SZ_2M), SZ_2M, 0,
					 DRM_MM_INSERT_BEST);
	spin_unlock(&vgdev->host_visible_lock);

	if (ret)
		return ret;

	objs = virtio_gpu_array_alloc(1);
	if (!objs) {
		ret = -ENOMEM;
		goto err_remove_node;
	}

	virtio_gpu_array_add_obj(objs, &bo->base.base);
	/*TODO: Add an error checking helper function in drm_mm.h */
	offset = vram->vram_node.start - vgdev->host_visible_region.addr;

	ret = virtio_gpu_cmd_map(vgdev, objs, offset);
	if (ret) {
		virtio_gpu_array_put_free(objs);
		goto err_remove_node;
	}

	return 0;

err_remove_node:
	spin_lock(&vgdev->host_visible_lock);
	drm_mm_remove_node(&vram->vram_node);
	spin_unlock(&vgdev->host_visible_lock);
	return ret;
}

int virtio_gpu_vram_create(struct virtio_gpu_device *vgdev,
			   struct virtio_gpu_object_params *params,
			   struct virtio_gpu_object **bo_ptr)
{
	struct drm_gem_object *obj;
	struct virtio_gpu_object_vram *vram;
	int ret;

	vram = kzalloc_obj(*vram);
	if (!vram)
		return -ENOMEM;

	obj = &vram->base.base.base;
	obj->funcs = &virtio_gpu_vram_funcs;

	params->size = PAGE_ALIGN(params->size);
	drm_gem_private_object_init(vgdev->ddev, obj, params->size);

	/* Create fake offset */
	ret = drm_gem_create_mmap_offset(obj);
	if (ret) {
		pr_err("VGBLOB-DBG: drm_gem_create_mmap_offset FAILED ret=%d size=%llu\n",
		       ret, (unsigned long long)params->size);
		kfree(vram);
		return ret;
	}

	ret = virtio_gpu_resource_id_get(vgdev, &vram->base.hw_res_handle);
	if (ret) {
		pr_err("VGBLOB-DBG: resource_id_get FAILED ret=%d\n", ret);
		kfree(vram);
		return ret;
	}

	virtio_gpu_cmd_resource_create_blob(vgdev, &vram->base, params, NULL,
					    0);
	if (params->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE) {
		ret = virtio_gpu_vram_map(&vram->base);
		if (ret) {
			pr_err("VGBLOB-DBG: vram_map FAILED ret=%d size=%llu (host_visible_mm full?)\n",
			       ret, (unsigned long long)params->size);
			virtio_gpu_vram_free(obj);
			return ret;
		}
	}

	*bo_ptr = &vram->base;
	return 0;
}

/* ===================================================================
 * DroidVM guest-alloc pool (gpu_guest_reserved)
 *
 * A page-granular bitmap allocator over the boot-blessed "gpu_guest_reserved"
 * region. The guest driver OWNS this pool: BLOB_MEM_GUEST blobs are backed by
 * pages carved from it, and the pool GPAs are handed to the host as ordinary
 * mem-entries. Because the region is SHARE-blessed (host-accessible, unlike
 * arbitrary guest RAM), the host resolves those entries via the official
 * attach_iov path -- so guest-alloc works in a protected VM.
 *
 * Increment 1: single contiguous run per blob (the pool is fresh/large, so
 * contiguity holds); a multi-run scatter-gather variant (matching upstream
 * shmem fragmentation) is a follow-up. Mapping is still the inherited
 * (write-combine) pgprot; cacheable is a follow-up.
 * =================================================================== */

int virtio_gpu_guest_pool_init(struct virtio_gpu_device *vgdev)
{
	struct device_node *rmem, *child;
	struct resource res;
	phys_addr_t base = 0;
	u64 size = 0;

	rmem = of_find_node_by_path("/reserved-memory");
	if (!rmem)
		return 0;
	for_each_child_of_node(rmem, child) {
		if (!of_node_name_prefix(child, "gpu_guest_reserved"))
			continue;
		if (of_address_to_resource(child, 0, &res) == 0) {
			base = res.start;
			size = resource_size(&res);
			of_node_put(child);
			break;
		}
	}
	of_node_put(rmem);

	if (!base || !size)
		return 0; /* guest-alloc pool absent -> feature off */

	mutex_init(&vgdev->guest_pool_lock);
	vgdev->guest_pool_npages = size >> PAGE_SHIFT;
	vgdev->guest_pool_bitmap = bitmap_zalloc(vgdev->guest_pool_npages,
						 GFP_KERNEL);
	if (!vgdev->guest_pool_bitmap)
		return -ENOMEM;

	vgdev->gpu_guest_pool_base = base;
	vgdev->gpu_guest_pool_size = size;
	pr_info("virtio-gpu: guest-alloc pool: base %pa size %llu MiB (%llu pages)\n",
		&vgdev->gpu_guest_pool_base, size >> 20,
		vgdev->guest_pool_npages);
	return 0;
}

void virtio_gpu_guest_pool_fini(struct virtio_gpu_device *vgdev)
{
	if (vgdev->guest_pool_bitmap) {
		bitmap_free(vgdev->guest_pool_bitmap);
		vgdev->guest_pool_bitmap = NULL;
	}
}

s64 virtio_gpu_guest_pool_alloc(struct virtio_gpu_device *vgdev, u64 npages)
{
	unsigned long start;

	if (!vgdev->guest_pool_bitmap || !npages)
		return -1;

	mutex_lock(&vgdev->guest_pool_lock);
	start = bitmap_find_next_zero_area(vgdev->guest_pool_bitmap,
					   vgdev->guest_pool_npages, 0,
					   npages, 0);
	if (start >= vgdev->guest_pool_npages) {
		mutex_unlock(&vgdev->guest_pool_lock);
		return -1; /* pool exhausted (or too fragmented for a single run) */
	}
	bitmap_set(vgdev->guest_pool_bitmap, start, npages);
	mutex_unlock(&vgdev->guest_pool_lock);

	return (s64)start << PAGE_SHIFT;
}

void virtio_gpu_guest_pool_free(struct virtio_gpu_device *vgdev, u64 offset,
				u64 npages)
{
	unsigned long start = offset >> PAGE_SHIFT;

	if (!vgdev->guest_pool_bitmap || !npages)
		return;

	mutex_lock(&vgdev->guest_pool_lock);
	bitmap_clear(vgdev->guest_pool_bitmap, start, npages);
	mutex_unlock(&vgdev->guest_pool_lock);
}

int virtio_gpu_guest_pool_create(struct virtio_gpu_device *vgdev,
				 struct virtio_gpu_object_params *params,
				 struct virtio_gpu_object **bo_ptr)
{
	struct virtio_gpu_object_vram *vram;
	struct virtio_gpu_mem_entry *ents;
	struct drm_gem_object *obj;
	u64 npages;
	s64 offset;
	int ret;

	params->size = PAGE_ALIGN(params->size);
	npages = params->size >> PAGE_SHIFT;

	offset = virtio_gpu_guest_pool_alloc(vgdev, npages);
	if (offset < 0) {
		pr_err("VGBLOB-DBG: guest-alloc pool OOM size=%llu (npages=%llu)\n",
		       (unsigned long long)params->size, npages);
		return -ENOMEM;
	}

	vram = kzalloc(sizeof(*vram), GFP_KERNEL);
	if (!vram) {
		ret = -ENOMEM;
		goto err_pool;
	}

	obj = &vram->base.base.base;
	obj->funcs = &virtio_gpu_vram_funcs;
	drm_gem_private_object_init(vgdev->ddev, obj, params->size);

	ret = drm_gem_create_mmap_offset(obj);
	if (ret) {
		pr_err("VGBLOB-DBG: guest-alloc mmap_offset FAILED ret=%d\n", ret);
		goto err_obj;
	}

	ret = virtio_gpu_resource_id_get(vgdev, &vram->base.hw_res_handle);
	if (ret)
		goto err_obj;

	/*
	 * Single contiguous mem-entry pointing into the guest-alloc pool. The host
	 * resolves it via get_slice_at_addr (the GpuPoolGuest region is host-accessible),
	 * i.e. the ordinary attach_iov path -- no host-side pool allocator involved.
	 */
	ents = kmalloc(sizeof(*ents), GFP_KERNEL);
	if (!ents) {
		ret = -ENOMEM;
		goto err_obj;
	}
	ents[0].addr = cpu_to_le64(vgdev->gpu_guest_pool_base + (u64)offset);
	ents[0].length = cpu_to_le32(params->size);
	ents[0].padding = 0;

	vram->pool_resident = true;
	vram->guest_pool_owned = true;
	vram->pool_offset = (u64)offset;
	/*
	 * The gfx-guest pool is SHARE'd (not lent) Normal-cacheable RAM in stage-2, so the guest
	 * may map it cacheable. WC read-back is very slow and zink/mutter read host-visible buffers,
	 * so cache these mappings; the mmap path (virtio_gpu_vram_mmap) leaves the default cacheable
	 * pgprot for CACHE_CACHED. Coherence with the host GPU relies on the imported udmabuf being
	 * I/O-coherent (Adreno SMMU); if artifacts appear, a non-coherent HOST_CACHED + flush path is
	 * the fallback.
	 */
	vram->map_info = VIRTIO_GPU_MAP_CACHE_CACHED;
	vram->vram_node.size = params->size;      /* mmap bounds check only (no drm_mm insert) */

	virtio_gpu_cmd_resource_create_blob(vgdev, &vram->base, params, ents, 1);
	virtio_gpu_notify(vgdev);

	*bo_ptr = &vram->base;
	return 0;

err_obj:
	/* Mirror virtio_gpu_vram_create's teardown (the resource id, if taken, leaks the
	 * same way it does there -- resource_id_put is file-local to virtgpu_object.c). */
	drm_gem_object_release(obj);
	kfree(vram);
err_pool:
	virtio_gpu_guest_pool_free(vgdev, (u64)offset, npages);
	return ret;
}
