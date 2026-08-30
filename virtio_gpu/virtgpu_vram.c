// SPDX-License-Identifier: GPL-2.0
#include "virtgpu_drv.h"

#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sizes.h>
#include <linux/gunyah_guest.h>

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

		/* Guest-pool blocks are returned from virtio_gpu_cmd_unref_cb, after the host's
		 * RESOURCE_UNREF response confirms that crosvm has dropped its pool reference. */
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
	 * guest-alloc: the backing is a list of pool blocks, so stitch them into one contiguous
	 * user mapping -- block order is the order they were allocated in, which is also the order
	 * the mem-entries went to the host, so offset N in this mapping is offset N there too.
	 *
	 * vm_pgoff is an offset into the object (a partial mmap), so walk past whole blocks that
	 * fall before it and start part-way into the one that straddles it.
	 */
	if (vram->guest_pool_owned) {
		unsigned long uaddr = vma->vm_start;
		u64 skip = (u64)vma->vm_pgoff << PAGE_SHIFT;
		struct drm_buddy_block *block;

		if (!vgdev->gpu_guest_pool_base) {
			pr_err("virtio-gpu: guest-alloc blob but no gpu_guest base in DT\n");
			return -EINVAL;
		}

		list_for_each_entry(block, &vram->guest_pool_blocks, link) {
			u64 off = drm_buddy_block_offset(block);
			u64 len = drm_buddy_block_size(&vgdev->guest_pool_mm, block);
			phys_addr_t pa;

			if (skip >= len) {
				skip -= len;
				continue;
			}
			off += skip;
			len -= skip;
			skip = 0;

			if (len > (u64)(vma->vm_end - uaddr))
				len = vma->vm_end - uaddr;
			if (!len)
				break;

			pa = vgdev->gpu_guest_pool_base + off;
			ret = io_remap_pfn_range(vma, uaddr, pa >> PAGE_SHIFT, len,
						 vma->vm_page_prot);
			if (ret)
				return ret;

			uaddr += len;
			if (uaddr >= vma->vm_end)
				break;
		}
		if (uaddr < vma->vm_end) {
			pr_err("virtio-gpu: guest-alloc mmap short by %lu bytes\n",
			       vma->vm_end - uaddr);
			return -EINVAL;
		}
		return 0;
	}

	/*
	 * gfxstream pre-alloc (host pool): still one run, so remap gpu_pool_base + pool_offset
	 * instead of the BAR node. The pool is already in the guest stage-2, so no accept was
	 * needed.
	 */
	if (vram->pool_resident) {
		phys_addr_t pa;

		if (!vgdev->gpu_pool_base) {
			pr_err("virtio-gpu: pool-resident blob but no pool base in DT\n");
			return -EINVAL;
		}
		pa = vgdev->gpu_pool_base + vram->pool_offset;
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
	struct drm_buddy_block *block;
	struct scatterlist *sg;
	struct sg_table *sgt;
	dma_addr_t addr;
	u32 nents = 0;
	unsigned int i, mapped = 0;
	int ret;

	sgt = kzalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	if (vram->guest_pool_owned) {
		/* Guest-pool blobs are scatter-backed. Mapping vram_node.start here would map
		 * physical address zero because guest-pool objects do not reserve a drm_mm node. */
		list_for_each_entry(block, &vram->guest_pool_blocks, link)
			nents++;
		if (!nents) {
			ret = -EINVAL;
			goto out;
		}

		ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
		if (ret)
			goto out;

		/* Build one DMA segment per buddy block. The block list and sg list have the same
		 * order, so a single cursor for each is sufficient. */
		sg = sgt->sgl;
		list_for_each_entry(block, &vram->guest_pool_blocks, link) {
			phys_addr_t pa = vgdev->gpu_guest_pool_base +
				drm_buddy_block_offset(block);
			u64 len = drm_buddy_block_size(&vgdev->guest_pool_mm, block);

			addr = dma_map_resource(dev, pa, len, dir,
						DMA_ATTR_SKIP_CPU_SYNC);
			if (dma_mapping_error(dev, addr)) {
				ret = -EIO;
				goto err_unmap_guest;
			}
			sg_set_page(sg, NULL, len, 0);
			sg_dma_address(sg) = addr;
			sg_dma_len(sg) = len;
			mapped++;
			sg = sg_next(sg);
		}
		return sgt;

err_unmap_guest:
		for_each_sg(sgt->sgl, sg, mapped, i) {
			if (sg_dma_len(sg))
				dma_unmap_resource(dev, sg_dma_address(sg), sg_dma_len(sg), dir,
						   DMA_ATTR_SKIP_CPU_SYNC);
		}
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(ret);
	}

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
	struct scatterlist *sg;
	unsigned int i;

	for_each_sgtable_sg(sgt, sg, i) {
		if (sg_dma_len(sg))
			dma_unmap_resource(dev, sg_dma_address(sg), sg_dma_len(sg), dir,
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
 * DroidVM guest-alloc pool (gpu_guest)
 *
 * The guest owns the drm_buddy allocator and hands the host pool GPAs as ordinary mem-entries.
 * A dynamic pool is declared at its full size in DT but only has a SHARE'd prefix initially;
 * gpu_guest_pool_backed is the access boundary. Extending that boundary is a synchronous pool
 * SHARE, and shrinking is restricted to completely free suffix steps so no live BO moves.
 * =================================================================== */

static void virtio_gpu_guest_pool_reclaim_work(struct work_struct *work);
static bool virtio_gpu_guest_pool_reclaim_locked(struct virtio_gpu_device *vgdev);

static int virtio_gpu_guest_pool_grow_one_locked(struct virtio_gpu_device *vgdev)
{
	u64 offset;
	bool range_backed = false;
	int ret;

	if (!vgdev->gpu_guest_pool_step ||
	    vgdev->gpu_guest_pool_backed >= vgdev->gpu_guest_pool_size)
		return -ENOSPC;
	if (vgdev->gpu_guest_pool_step >
	    vgdev->gpu_guest_pool_size - vgdev->gpu_guest_pool_backed)
		return -EINVAL;

	offset = vgdev->gpu_guest_pool_backed;
	ret = gunyah_pool_grow(vgdev->gpu_guest_pool_id, offset,
				       vgdev->gpu_guest_pool_step);
	if (ret == -ETIMEDOUT || ret == -EEXIST) {
		/* The host may have completed SHARE before its response was lost. */
		int query_ret = gunyah_pool_query_range(
			vgdev->gpu_guest_pool_id, offset,
			vgdev->gpu_guest_pool_step, &range_backed);

		if (!query_ret && range_backed)
			ret = 0;
	}
	if (!ret)
		vgdev->gpu_guest_pool_backed += vgdev->gpu_guest_pool_step;

	return ret;
}

int virtio_gpu_guest_pool_init(struct virtio_gpu_device *vgdev)
{
	struct device_node *rmem, *child;
	struct resource res;
	phys_addr_t base = 0;
	u64 size = 0, prealloc = 0, step = 0;
	u32 pool_id = 0;
	int ret;

	INIT_DELAYED_WORK(&vgdev->guest_pool_reclaim_work,
			  virtio_gpu_guest_pool_reclaim_work);

	rmem = of_find_node_by_path("/reserved-memory");
	if (!rmem)
		return 0;
	for_each_child_of_node(rmem, child) {
		if (!of_node_name_prefix(child, "gpu_guest"))
			continue;
		if (of_address_to_resource(child, 0, &res) == 0) {
			base = res.start;
			/*
			 * `reg` covers the pre-shared floor, not the whole window: the Gunyah
			 * resource manager on android14-6.1 refuses to start a VM whose
			 * reserved-memory node describes a range no memparcel matches, and only
			 * the floor is a memparcel before boot. The window's real size therefore
			 * arrives beside it. A pool that is fully pre-shared omits the property
			 * because for it the floor IS the window, which is also what every DT
			 * built before this existed looks like.
			 */
			if (of_property_read_u64(child, "droidvm,pool-size", &size))
				size = resource_size(&res);
			if (of_property_read_u64(child, "droidvm,pre-alloc-size", &prealloc))
				prealloc = resource_size(&res);
			of_property_read_u64(child, "droidvm,step-size", &step);
			of_property_read_u32(child, "droidvm,pool-id", &pool_id);
			of_node_put(child);
			break;
		}
	}
	of_node_put(rmem);

	if (!base || !size) {
		/*
		 * No pool: guest-alloc blobs fall back to ordinary shmem. That is the correct
		 * behaviour on a VMM whose host can read guest RAM directly -- plain KVM -- and
		 * it is why the fallback exists rather than failing the allocation.
		 *
		 * It is NOT correct where guest RAM is lent rather than shared, because there the
		 * host cannot reach those pages at all and the GPU ends up bound to memory the
		 * guest thinks is private. A restricted-dma-pool in the device tree is that
		 * platform's fingerprint, so say so loudly there and quietly everywhere else.
		 * Diagnosing this from the host side means noticing that blob iovecs point below
		 * the pool base, which is a long way from the symptom.
		 */
		struct device_node *rdma =
			of_find_compatible_node(NULL, NULL, "restricted-dma-pool");

		if (rdma) {
			pr_warn("virtio-gpu: guest-alloc: no pool in DT, falling back to shmem -- but this VM lends its RAM (restricted-dma-pool present), so the host cannot reach it. Expect the GPU to read the wrong memory. Missing --pre-alloc gpu-guest-mb?\n");
			of_node_put(rdma);
		} else {
			pr_info("virtio-gpu: guest-alloc: no pool in DT, backing blobs from shmem\n");
		}
		return 0;
	}

	/* drm_buddy wants a chunk-aligned size; trim rather than round up, the tail is not ours. */
	size = ALIGN_DOWN(size, PAGE_SIZE);
	if (!size)
		return 0;
	if (prealloc > size || !IS_ALIGNED(prealloc, PAGE_SIZE)) {
		pr_err("virtio-gpu: guest-alloc pool: invalid prealloc %#llx for size %#llx\n",
		       prealloc, size);
		return -EINVAL;
	}
	/*
	 * A step is a multiple of a 2 MiB folio, which is what the host shares a grant in -- not a
	 * power of two. drm_buddy never sees it: this pool is initialised with PAGE_SIZE as its
	 * chunk (below), so the allocator has no opinion about the grant granularity at all.
	 */
	if (step && (step % SZ_2M || !IS_ALIGNED(size, step) ||
		     !IS_ALIGNED(prealloc, step))) {
		pr_err("virtio-gpu: guest-alloc pool: invalid prealloc/step size %#llx/%#llx\n",
		       prealloc, step);
		return -EINVAL;
	}
	if (!step && prealloc != size) {
		pr_err("virtio-gpu: guest-alloc pool: partial prealloc requires a non-zero step\n");
		return -EINVAL;
	}

	mutex_init(&vgdev->guest_pool_lock);
	ret = drm_buddy_init(&vgdev->guest_pool_mm, size, PAGE_SIZE);
	if (ret) {
		pr_err("virtio-gpu: guest-alloc pool: drm_buddy_init failed %d\n", ret);
		return ret;
	}
	vgdev->guest_pool_ready = true;

	vgdev->gpu_guest_pool_base = base;
	vgdev->gpu_guest_pool_size = size;
	vgdev->gpu_guest_pool_id = pool_id;
	vgdev->gpu_guest_pool_prealloc = prealloc;
	vgdev->gpu_guest_pool_backed = prealloc;
	vgdev->gpu_guest_pool_step = step;
	pr_info("virtio-gpu: guest-alloc pool: base %pa size %llu MiB prealloc %llu MiB step %llu MiB id %u (drm_buddy, %u roots, max order %u)\n",
		&vgdev->gpu_guest_pool_base, size >> 20,
		prealloc >> 20, step >> 20, pool_id,
		vgdev->guest_pool_mm.n_roots, vgdev->guest_pool_mm.max_order);
	return 0;
}

void virtio_gpu_guest_pool_fini(struct virtio_gpu_device *vgdev)
{
	cancel_delayed_work_sync(&vgdev->guest_pool_reclaim_work);
	if (!READ_ONCE(vgdev->guest_pool_ready))
		return;

	/* Serialize with alloc/free and close the gate before destroying the buddy tree. Late
	 * RESOURCE_UNREF callbacks may still arrive after virtio_reset_device(); their release path
	 * must see the closed gate and leave its block list untouched rather than dereferencing a dead
	 * allocator. */
	mutex_lock(&vgdev->guest_pool_lock);
	if (vgdev->guest_pool_ready) {
		u64 live = vgdev->guest_pool_mm.size - vgdev->guest_pool_mm.avail;

		WRITE_ONCE(vgdev->guest_pool_ready, false);
		if (live)
			pr_warn("virtio-gpu: guest-alloc pool teardown with %llu bytes still allocated\n",
				live);
		drm_buddy_fini(&vgdev->guest_pool_mm);
	}
	mutex_unlock(&vgdev->guest_pool_lock);
}

/* Carve `size` bytes out of the pool as a list of blocks.
 *
 * No contiguity is asked for: the caller sends one mem-entry per block and stitches them back
 * together in user VA at mmap time, so a fragmented pool is still a usable one. Callers that
 * genuinely need one physical run (scanout) would pass DRM_BUDDY_CONTIGUOUS_ALLOCATION; none do
 * today.
 */
int virtio_gpu_guest_pool_alloc(struct virtio_gpu_device *vgdev, u64 size,
				struct list_head *blocks)
{
	u64 min_bs = PAGE_SIZE;
	u64 request_size;
	bool retry_reclaim = false;
	int ret = -ENOMEM;

	INIT_LIST_HEAD(blocks);
	if (!READ_ONCE(vgdev->guest_pool_ready) || !size)
		return -ENOMEM;

	size = ALIGN(size, PAGE_SIZE);

	request_size = size;
	if (request_size > vgdev->gpu_guest_pool_size)
		return -ENOMEM;

	mutex_lock(&vgdev->guest_pool_lock);
	if (!vgdev->guest_pool_ready) {
		mutex_unlock(&vgdev->guest_pool_lock);
		return -ENODEV;
	}
	for (;;) {
		/* Do not even ask drm_buddy to inspect the ungranted suffix. A read from an
		 * ungranted protected-VM address is silently zero rather than a recoverable fault. */
		ret = 0;
		while (vgdev->gpu_guest_pool_backed < request_size) {
			if (!vgdev->gpu_guest_pool_step) {
				ret = -ENOMEM;
				break;
			}
			ret = virtio_gpu_guest_pool_grow_one_locked(vgdev);
			if (ret)
				break;
		}
		if (ret)
			break;

		ret = drm_buddy_alloc_blocks(&vgdev->guest_pool_mm, 0,
					     vgdev->gpu_guest_pool_backed, size, min_bs,
					     blocks, DRM_BUDDY_RANGE_ALLOCATION);
		if (!ret || ret != -ENOSPC ||
		    !vgdev->gpu_guest_pool_step ||
		    vgdev->gpu_guest_pool_backed >= vgdev->gpu_guest_pool_size)
			break;

		/* Fragmentation can make the currently backed prefix fail even though the
		 * full pool has room. Add one suffix step and retry in the same lock. */
		ret = virtio_gpu_guest_pool_grow_one_locked(vgdev);
		if (ret)
			break;
	}
	/* A failed allocation may have grown one or more otherwise-unused suffix steps. Return
	 * those steps immediately instead of making an OOM attempt permanently raise the floor. */
	if (ret)
		retry_reclaim = virtio_gpu_guest_pool_reclaim_locked(vgdev);
	mutex_unlock(&vgdev->guest_pool_lock);
	if (retry_reclaim)
		schedule_delayed_work(&vgdev->guest_pool_reclaim_work,
				      msecs_to_jiffies(100));

	return ret;
}

static int virtio_gpu_guest_pool_shrink_one_locked(struct virtio_gpu_device *vgdev)
{
	u64 offset;
	bool range_backed = true;
	int ret;

	if (!vgdev->gpu_guest_pool_step ||
	    vgdev->gpu_guest_pool_backed <= vgdev->gpu_guest_pool_prealloc)
		return -ENOSPC;

	offset = vgdev->gpu_guest_pool_backed - vgdev->gpu_guest_pool_step;
	ret = gunyah_pool_shrink(vgdev->gpu_guest_pool_id, offset,
				 vgdev->gpu_guest_pool_step);
	if (ret) {
		/* A timeout does not say whether the host completed the reclaim. Query the
		 * exact range before deciding whether it is safe to lower our boundary. */
		int query_ret = gunyah_pool_query_range(
			vgdev->gpu_guest_pool_id, offset,
			vgdev->gpu_guest_pool_step, &range_backed);
		if (!query_ret && !range_backed)
			ret = 0;
	}
	if (ret == -EUCLEAN) {
		/* The host kept the grant for retry, but could not restore this guest mapping after
		 * releasing it. Keep only the already-safe prefix and permanently stop growing this
		 * pool. Leaking the host grant is preferable to touching an unmapped suffix. */
		pr_err("virtio-gpu: guest-alloc pool: host/guest mapping recovery failed at %#llx; disabling further growth\n",
		       offset);
		vgdev->gpu_guest_pool_backed = offset;
		vgdev->gpu_guest_pool_step = 0;
		return 0;
	}
	if (!ret)
		vgdev->gpu_guest_pool_backed = offset;
	return ret;
}

/* Probe one suffix step by allocating and immediately freeing the exact range. The buddy tree
 * remains unchanged, but this tells us whether any live BO block overlaps the step. */
static bool virtio_gpu_guest_pool_step_free_locked(struct virtio_gpu_device *vgdev)
{
	LIST_HEAD(probe);
	u64 offset;

	if (!vgdev->gpu_guest_pool_step ||
	    vgdev->gpu_guest_pool_backed <= vgdev->gpu_guest_pool_prealloc)
		return false;

	offset = vgdev->gpu_guest_pool_backed - vgdev->gpu_guest_pool_step;
	if (drm_buddy_alloc_blocks(&vgdev->guest_pool_mm, offset,
				   offset + vgdev->gpu_guest_pool_step,
				   vgdev->gpu_guest_pool_step,
				   vgdev->gpu_guest_pool_step, &probe,
				   DRM_BUDDY_RANGE_ALLOCATION))
		return false;

	droidvm_drm_buddy_free_list(&vgdev->guest_pool_mm, &probe);
	return true;
}

static bool virtio_gpu_guest_pool_reclaim_locked(struct virtio_gpu_device *vgdev)
{
	int ret;

	while (virtio_gpu_guest_pool_step_free_locked(vgdev)) {
		ret = virtio_gpu_guest_pool_shrink_one_locked(vgdev);
		if (!ret)
			continue;

		/* EIO is retryable when the host restored the mapping after a failed punch;
		 * EUCLEAN is deliberately handled by shrink_one_locked as a terminal state. */
		if (ret == -EBUSY || ret == -ETIMEDOUT || ret == -EIO)
			return true;
		pr_warn_ratelimited("virtio-gpu: guest-alloc pool shrink failed: %d\n", ret);
		break;
	}
	return false;
}

static void virtio_gpu_guest_pool_reclaim_work(struct work_struct *work)
{
	struct virtio_gpu_device *vgdev = container_of(
		to_delayed_work(work), struct virtio_gpu_device, guest_pool_reclaim_work);
	bool retry = false;

	mutex_lock(&vgdev->guest_pool_lock);
	if (vgdev->guest_pool_ready)
		retry = virtio_gpu_guest_pool_reclaim_locked(vgdev);
	mutex_unlock(&vgdev->guest_pool_lock);

	if (retry)
		schedule_delayed_work(&vgdev->guest_pool_reclaim_work,
				      msecs_to_jiffies(100));
}

void virtio_gpu_guest_pool_free(struct virtio_gpu_device *vgdev, struct list_head *blocks)
{
	bool retry;

	if (!READ_ONCE(vgdev->guest_pool_ready) || list_empty(blocks))
		return;

	mutex_lock(&vgdev->guest_pool_lock);
	if (!vgdev->guest_pool_ready) {
		mutex_unlock(&vgdev->guest_pool_lock);
		return;
	}
	droidvm_drm_buddy_free_list(&vgdev->guest_pool_mm, blocks);
	retry = virtio_gpu_guest_pool_reclaim_locked(vgdev);
	mutex_unlock(&vgdev->guest_pool_lock);
	if (retry)
		schedule_delayed_work(&vgdev->guest_pool_reclaim_work,
			      msecs_to_jiffies(100));
}

void virtio_gpu_guest_pool_release_object(struct virtio_gpu_device *vgdev,
					  struct virtio_gpu_object *bo)
{
	struct virtio_gpu_object_vram *vram;

	if (!virtio_gpu_is_vram(bo))
		return;
	vram = to_virtio_gpu_vram(bo);
	if (!vram->guest_pool_owned)
		return;
	vram->guest_pool_owned = false;
	virtio_gpu_guest_pool_free(vgdev, &vram->guest_pool_blocks);
}

/* Pool occupancy, for the VK_EXT_memory_budget figures the guest ICD reports.
 *
 * largest_free is "the largest allocation that can succeed", which under drm_buddy is simply the
 * free total: an allocation is a list of blocks, so any chunk-aligned size that fits at all fits.
 * It is reported separately anyway because that equality is a property of this allocator, not of
 * the interface -- the page bitmap this replaced could not satisfy a request larger than its
 * biggest contiguous run, and a caller should not have to know which one is underneath.
 */
void virtio_gpu_guest_pool_stats(struct virtio_gpu_device *vgdev, u64 *total_bytes,
				 u64 *used_bytes, u64 *largest_free_bytes)
{
	*total_bytes = 0;
	*used_bytes = 0;
	*largest_free_bytes = 0;

	if (!READ_ONCE(vgdev->guest_pool_ready))
		return;

	mutex_lock(&vgdev->guest_pool_lock);
	if (vgdev->guest_pool_ready) {
		*total_bytes = vgdev->guest_pool_mm.size;
		*used_bytes = vgdev->guest_pool_mm.size - vgdev->guest_pool_mm.avail;
		*largest_free_bytes = vgdev->guest_pool_mm.avail;
	}
	mutex_unlock(&vgdev->guest_pool_lock);
}

int virtio_gpu_guest_pool_create(struct virtio_gpu_device *vgdev,
				 struct virtio_gpu_object_params *params,
				 struct virtio_gpu_object **bo_ptr)
{
	struct virtio_gpu_object_vram *vram;
	struct virtio_gpu_mem_entry *ents;
	struct drm_buddy_block *block;
	struct drm_gem_object *obj;
	struct list_head blocks;
	u32 nents = 0, i = 0;
	int ret;

	params->size = PAGE_ALIGN(params->size);

	ret = virtio_gpu_guest_pool_alloc(vgdev, params->size, &blocks);
	if (ret) {
		u64 total, used, largest;

		virtio_gpu_guest_pool_stats(vgdev, &total, &used, &largest);
		pr_err("VGBLOB-DBG: guest-alloc pool OOM comm=%s pid=%d size=%llu (pool %llu MiB, used %llu MiB, largest %llu MiB)\n",
		       current->comm, current->pid,
		       (unsigned long long)params->size, total >> 20, used >> 20,
		       largest >> 20);
		return -ENOMEM;
	}

	vram = kzalloc(sizeof(*vram), GFP_KERNEL);
	if (!vram) {
		ret = -ENOMEM;
		goto err_pool;
	}
	INIT_LIST_HEAD(&vram->guest_pool_blocks);
	list_splice_init(&blocks, &vram->guest_pool_blocks);

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
	 * One mem-entry per pool block. The host resolves each via get_slice_at_addr (the
	 * GpuPoolGuest region is host-accessible), i.e. the ordinary attach_iov path -- no
	 * host-side pool allocator involved, and crosvm already builds its udmabuf from however
	 * many segments arrive. kvmalloc because a badly fragmented pool can make this list long
	 * enough to matter, even though the buddy structure keeps it near log2 in practice.
	 */
	list_for_each_entry(block, &vram->guest_pool_blocks, link)
		nents++;

	if (nents > 1 && !vgdev->guest_pool_multiblock_seen) {
		vgdev->guest_pool_multiblock_seen = true;
		pr_info("virtio-gpu: guest-alloc: scatter allocation in use (%u blocks for %lu bytes)\n",
			nents, params->size);
	}

	ents = kvmalloc_array(nents, sizeof(*ents), GFP_KERNEL);
	if (!ents) {
		ret = -ENOMEM;
		goto err_obj;
	}
	list_for_each_entry(block, &vram->guest_pool_blocks, link) {
		ents[i].addr = cpu_to_le64(vgdev->gpu_guest_pool_base +
					   drm_buddy_block_offset(block));
		ents[i].length = cpu_to_le32(drm_buddy_block_size(&vgdev->guest_pool_mm,
								 block));
		ents[i].padding = 0;
		i++;
	}

	vram->pool_resident = true;
	vram->guest_pool_owned = true;
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

	virtio_gpu_cmd_resource_create_blob(vgdev, &vram->base, params, ents, nents);
	virtio_gpu_notify(vgdev);

	*bo_ptr = &vram->base;
	return 0;

err_obj:
	/* Mirror virtio_gpu_vram_create's teardown (the resource id, if taken, leaks the
	 * same way it does there -- resource_id_put is file-local to virtgpu_object.c). */
	virtio_gpu_guest_pool_free(vgdev, &vram->guest_pool_blocks);
	drm_gem_object_release(obj);
	kfree(vram);
	return ret;
err_pool:
	virtio_gpu_guest_pool_free(vgdev, &blocks);
	return ret;
}
