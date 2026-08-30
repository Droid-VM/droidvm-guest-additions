/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef VIRTIO_DRV_H
#define VIRTIO_DRV_H

#include <linux/dma-direction.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/virtio_gpu.h>
/*
 * DroidVM gfxstream pre-alloc: the build uses the kernel's <linux/virtio_gpu.h> (not the
 * vendored uapi copy), so define our map_info flag here where every .c file sees it. Set in
 * the RESOURCE_MAP_BLOB response's map_info when the blob is GpuPool-resident; the response's
 * padding field then carries the pool byte offset.
 */
#ifndef VIRTIO_GPU_MAP_INFO_POOL
#define VIRTIO_GPU_MAP_INFO_POOL      (1u << 31)
#endif

#include <drm/drm_atomic.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_buddy.h>
#include <drm/drm_probe_helper.h>
#include <drm/virtgpu_drm.h>

#include <linux/version.h>

/* 7.1 moved the buddy allocator to the generic <linux/gpu_buddy.h> library (built-in
 * CONFIG_GPU_BUDDY, still selected by DRM_BUDDY): struct drm_buddy became struct gpu_buddy and
 * every drm_buddy_* symbol / DRM_BUDDY_* flag was renamed gpu_buddy_* / GPU_BUDDY_* 1:1 with
 * unchanged signatures. <drm/drm_buddy.h> still exists but only carries print helpers, so keep
 * the pre-7.1 spellings below and map them here. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0)
#define drm_buddy			gpu_buddy
#define drm_buddy_block			gpu_buddy_block
#define drm_buddy_init			gpu_buddy_init
#define drm_buddy_fini			gpu_buddy_fini
#define drm_buddy_alloc_blocks		gpu_buddy_alloc_blocks
#define drm_buddy_free_list		gpu_buddy_free_list
#define drm_buddy_block_offset		gpu_buddy_block_offset
#define drm_buddy_block_size		gpu_buddy_block_size
#define DRM_BUDDY_RANGE_ALLOCATION	GPU_BUDDY_RANGE_ALLOCATION
#define DRM_BUDDY_CONTIGUOUS_ALLOCATION	GPU_BUDDY_CONTIGUOUS_ALLOCATION
#define DRM_BUDDY_CLEAR_ALLOCATION	GPU_BUDDY_CLEAR_ALLOCATION
#endif

/* drm_buddy_free_list() gained the flags argument with clear-page tracking in 6.10. */
#ifdef DRM_BUDDY_CLEAR_ALLOCATION
#define droidvm_drm_buddy_free_list(mm, objects) \
	drm_buddy_free_list((mm), (objects), 0)
#else
#define droidvm_drm_buddy_free_list(mm, objects) \
	drm_buddy_free_list((mm), (objects))
#endif

/* DroidVM guest-alloc: crosvm's downstream virtio feature (bit 6) + getparam (10). The kernel's
 * <linux/virtio_gpu.h> / <drm/virtgpu_drm.h> predate them, and the fork's uapi copy is shadowed by
 * the kernel header's include guard -- so define them here (included by every driver TU). */
#ifndef VIRTIO_GPU_F_CREATE_GUEST_HANDLE
#define VIRTIO_GPU_F_CREATE_GUEST_HANDLE 6
#endif
#ifndef VIRTGPU_PARAM_CREATE_GUEST_HANDLE
#define VIRTGPU_PARAM_CREATE_GUEST_HANDLE 10
#endif

/* DroidVM guest-alloc pool accounting, for VK_EXT_memory_budget.
 *
 * In guest-alloc mode this driver owns the allocator, so nothing on the host side knows how full
 * the pool is: crosvm only ever sees one sglist per blob. Without these, gfxstream's budget
 * override bails out and the guest is told whatever turnip reports for the phone's system heap --
 * several GiB against a pool that may be one. A client that honours VK_EXT_memory_budget then
 * allocates until the pool hard-fails instead of backing off.
 *
 * KiB, not bytes: getparam copies out an int (upstream writes sizeof(int) through a u64 pointer),
 * which caps a byte count at 2 GiB but a KiB count at 2 TiB.
 *
 * Three separate queries rather than one struct because no invariant spans them -- total is fixed
 * after probe, and used and largest-free are each meaningful on their own -- so there is nothing
 * for a torn read to break. The high numbers keep them clear of the upstream range (which ends at
 * 10, itself already a crosvm downstream addition).
 *
 * LARGEST_FREE means "the largest allocation that can currently succeed", which is not always
 * total - used. Under the page-bitmap allocator this driver started with it was the largest
 * contiguous run, and fragmentation could fail an allocation with most of the pool free. Under
 * drm_buddy an allocation is a list of blocks, so any chunk-aligned size up to the free total can
 * be satisfied and the two figures coincide -- but the definition is the one that stays useful if
 * the allocator changes again, and it is what a client actually needs to know.
 */
#define VIRTGPU_PARAM_GUEST_POOL_TOTAL_KIB 0x1000
#define VIRTGPU_PARAM_GUEST_POOL_USED_KIB 0x1001
#define VIRTGPU_PARAM_GUEST_POOL_LARGEST_FREE_KIB 0x1002

#define DRIVER_NAME "virtio_gpu"
#define DRIVER_DESC "virtio GPU"

#define DRIVER_MAJOR 0
#define DRIVER_MINOR 1
#define DRIVER_PATCHLEVEL 0

#define STATE_INITIALIZING 0
#define STATE_OK 1
#define STATE_ERR 2

#define MAX_CAPSET_ID 63
#define MAX_RINGS 64

/* See virtio_gpu_ctx_create. One additional character for NULL terminator. */
#define DEBUG_NAME_MAX_LEN 65

struct virtio_gpu_object_params {
	unsigned long size;
	bool dumb;
	/* 3d */
	bool virgl;
	bool blob;

	/* classic resources only */
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t target;
	uint32_t bind;
	uint32_t depth;
	uint32_t array_size;
	uint32_t last_level;
	uint32_t nr_samples;
	uint32_t flags;

	/* blob resources only */
	uint32_t ctx_id;
	uint32_t blob_mem;
	uint32_t blob_flags;
	uint64_t blob_id;
};

struct virtio_gpu_object {
	struct drm_gem_shmem_object base;
	struct sg_table *sgt;
	uint32_t hw_res_handle;
	bool dumb;
	bool created;
	bool attached;
	bool host3d_blob, guest_blob;
	uint32_t blob_mem, blob_flags;

	int uuid_state;
	uuid_t uuid;
};
#define gem_to_virtio_gpu_obj(gobj) \
	container_of((gobj), struct virtio_gpu_object, base.base)

struct virtio_gpu_object_shmem {
	struct virtio_gpu_object base;
};

struct virtio_gpu_object_vram {
	struct virtio_gpu_object base;
	uint32_t map_state;
	uint32_t map_info;
	/*
	 * DroidVM gfxstream pre-alloc: this blob is GpuPool-resident. Its pages are already in
	 * the guest stage-2 (pool SHARE-blessed at boot), so mmap io_remaps gpu_pool_base +
	 * pool_offset with no runtime SHARE at all.
	 */
	bool pool_resident;
	u64 pool_offset;
	/*
	 * DroidVM guest-alloc: this pool-resident blob was sub-allocated by the GUEST driver
	 * from the gpu_guest pool (not the host). The guest built the mem-entries
	 * (pool GPAs) itself and must return the blocks to its own allocator on free.
	 *
	 * guest_pool_blocks holds them, in the order they were handed out -- the same order the
	 * mem-entries were built in and the same order mmap stitches them into user VA, so the
	 * guest and the host agree on which byte is where. pool_offset is meaningless for these
	 * (there is no single offset); it stays for host-pool blobs, which are still one run.
	 */
	bool guest_pool_owned;
	struct list_head guest_pool_blocks;
	struct drm_mm_node vram_node;
};

#define to_virtio_gpu_shmem(virtio_gpu_object) \
	container_of((virtio_gpu_object), struct virtio_gpu_object_shmem, base)

#define to_virtio_gpu_vram(virtio_gpu_object) \
	container_of((virtio_gpu_object), struct virtio_gpu_object_vram, base)

struct virtio_gpu_object_array {
	struct ww_acquire_ctx ticket;
	struct list_head next;
	u32 nents, total;
	struct drm_gem_object *objs[] __counted_by(total);
};

struct virtio_gpu_vbuffer;
struct virtio_gpu_device;

typedef void (*virtio_gpu_resp_cb)(struct virtio_gpu_device *vgdev,
				   struct virtio_gpu_vbuffer *vbuf);

struct virtio_gpu_fence_driver {
	atomic64_t       last_fence_id;
	uint64_t         current_fence_id;
	uint64_t         context;
	struct list_head fences;
	spinlock_t       lock;
};

struct virtio_gpu_fence_event {
	struct drm_pending_event base;
	struct drm_event event;
};

struct virtio_gpu_fence {
	struct dma_fence f;
	uint32_t ring_idx;
	uint64_t fence_id;
	bool emit_fence_info;
	struct virtio_gpu_fence_event *e;
	struct virtio_gpu_fence_driver *drv;
	struct list_head node;
};

struct virtio_gpu_vbuffer {
	char *buf;
	int size;

	void *data_buf;
	uint32_t data_size;

	char *resp_buf;
	int resp_size;
	virtio_gpu_resp_cb resp_cb;
	void *resp_cb_data;

	struct virtio_gpu_object_array *objs;
	struct list_head list;

	uint32_t seqno;
};

struct virtio_gpu_output {
	int index;
	struct drm_crtc crtc;
	struct drm_connector conn;
	struct drm_encoder enc;
	struct virtio_gpu_display_one info;
	struct virtio_gpu_update_cursor cursor;
	const struct drm_edid *drm_edid;
	int cur_x;
	int cur_y;
	bool needs_modeset;
};
#define drm_crtc_to_virtio_gpu_output(x) \
	container_of(x, struct virtio_gpu_output, crtc)

struct virtio_gpu_framebuffer {
	struct drm_framebuffer base;
	struct virtio_gpu_fence *fence;
};
#define to_virtio_gpu_framebuffer(x) \
	container_of(x, struct virtio_gpu_framebuffer, base)

struct virtio_gpu_plane_state {
	struct drm_plane_state base;
	struct virtio_gpu_fence *fence;
};
#define to_virtio_gpu_plane_state(x) \
	container_of(x, struct virtio_gpu_plane_state, base)

struct virtio_gpu_queue {
	struct virtqueue *vq;
	spinlock_t qlock;
	wait_queue_head_t ack_queue;
	struct work_struct dequeue_work;
	uint32_t seqno;
};

struct virtio_gpu_drv_capset {
	uint32_t id;
	uint32_t max_version;
	uint32_t max_size;
};

struct virtio_gpu_drv_cap_cache {
	struct list_head head;
	void *caps_cache;
	uint32_t id;
	uint32_t version;
	uint32_t size;
	atomic_t is_valid;
};

struct virtio_gpu_device {
	struct drm_device *ddev;

	struct virtio_device *vdev;

	struct virtio_gpu_output outputs[VIRTIO_GPU_MAX_SCANOUTS];
	uint32_t num_scanouts;

	struct virtio_gpu_queue ctrlq;
	struct virtio_gpu_queue cursorq;
	struct kmem_cache *vbufs;

	atomic_t pending_commands;

	struct ida	resource_ida;

	wait_queue_head_t resp_wq;
	/* current display info */
	spinlock_t display_info_lock;
	bool display_info_pending;

	struct virtio_gpu_fence_driver fence_drv;

	struct ida	ctx_id_ida;

	bool has_virgl_3d;
	bool has_edid;
	bool has_indirect;
	bool has_resource_assign_uuid;
	bool has_resource_blob;
	bool has_host_visible;
	bool has_context_init;
	/* DroidVM guest-alloc: VIRTIO_GPU_F_CREATE_GUEST_HANDLE negotiated (udmabuf=true). */
	bool has_create_guest_handle;
	struct virtio_shm_region host_visible_region;
	struct drm_mm host_visible_mm;
	/* DroidVM gfxstream pre-alloc: guest physical base of the boot-blessed GpuPool
	 * (from the /reserved-memory "gfx_host" DT node), or 0 if absent. A
	 * pool-resident blob maps gpu_pool_base + pool_offset directly. */
	phys_addr_t gpu_pool_base;

	/* DroidVM guest-alloc: the separate boot-blessed guest-alloc pool (from the
	 * "gpu_guest" DT node). The guest driver OWNS this region: it sub-allocates
	 * BLOB_MEM_GUEST backing from it (drm_buddy) and hands the pool GPAs to the
	 * host as ordinary mem-entries, so the official attach_iov path works in a protected VM
	 * (the pool is host-accessible, unlike arbitrary guest RAM). Zero base = guest-alloc off. */
	phys_addr_t gpu_guest_pool_base;
	u64 gpu_guest_pool_size;
	/*
	 * drm_buddy, not the page bitmap it replaced. The bitmap could only hand out one
	 * contiguous run (bitmap_find_next_zero_area), so an allocation failed as soon as the
	 * pool fragmented however much total space was free -- and a long session fragments.
	 * A buddy allocator returns a list of blocks instead, which is exactly the shape the
	 * rest of the path already wanted: virtio_gpu_mem_entry is an array, and the host side
	 * has always iterated the sglist. The power-of-two structure also bounds the number of
	 * blocks per allocation to roughly log2 rather than "however many holes exist", which
	 * keeps both the entry count and the mmap loop short.
	 *
	 * The lock is ours to hold: drm_buddy documents locking as the caller's job.
	 */
	struct drm_buddy guest_pool_mm;
	bool guest_pool_ready;
	/* Whether an allocation has ever needed more than one block. Says out loud that the
	 * scatter path is live rather than leaving it to be inferred from the absence of
	 * failures -- a single-block-only run would look identical from outside. */
	bool guest_pool_multiblock_seen;
	struct mutex guest_pool_lock;

	struct work_struct config_changed_work;

	struct work_struct obj_free_work;
	spinlock_t obj_free_lock;
	struct list_head obj_free_list;

	struct virtio_gpu_drv_capset *capsets;
	uint32_t num_capsets;
	uint64_t capset_id_mask;
	struct list_head cap_cache;

	/* protects uuid state when exporting */
	spinlock_t resource_export_lock;
	/* protects map state and host_visible_mm */
	spinlock_t host_visible_lock;
};

struct virtio_gpu_fpriv {
	uint32_t ctx_id;
	uint32_t context_init;
	bool context_created;
	uint32_t num_rings;
	uint64_t base_fence_ctx;
	uint64_t ring_idx_mask;
	struct mutex context_lock;
	char debug_name[DEBUG_NAME_MAX_LEN];
	bool explicit_debug_name;
};

/* virtgpu_ioctl.c */
#define DRM_VIRTIO_NUM_IOCTLS 12
extern struct drm_ioctl_desc virtio_gpu_ioctls[DRM_VIRTIO_NUM_IOCTLS];
void virtio_gpu_create_context(struct drm_device *dev, struct drm_file *file);

/* virtgpu_kms.c */
int virtio_gpu_init(struct virtio_device *vdev, struct drm_device *dev);
void virtio_gpu_deinit(struct drm_device *dev);
void virtio_gpu_release(struct drm_device *dev);
int virtio_gpu_driver_open(struct drm_device *dev, struct drm_file *file);
void virtio_gpu_driver_postclose(struct drm_device *dev, struct drm_file *file);

/* virtgpu_gem.c */
int virtio_gpu_gem_object_open(struct drm_gem_object *obj,
			       struct drm_file *file);
void virtio_gpu_gem_object_close(struct drm_gem_object *obj,
				 struct drm_file *file);
int virtio_gpu_mode_dumb_create(struct drm_file *file_priv,
				struct drm_device *dev,
				struct drm_mode_create_dumb *args);

struct virtio_gpu_object_array *virtio_gpu_panic_array_alloc(void);
struct virtio_gpu_object_array *virtio_gpu_array_alloc(u32 nents);
struct virtio_gpu_object_array*
virtio_gpu_array_from_handles(struct drm_file *drm_file, u32 *handles, u32 nents);
void virtio_gpu_array_add_obj(struct virtio_gpu_object_array *objs,
			      struct drm_gem_object *obj);
int virtio_gpu_array_lock_resv(struct virtio_gpu_object_array *objs);
void virtio_gpu_array_unlock_resv(struct virtio_gpu_object_array *objs);
void virtio_gpu_array_add_fence(struct virtio_gpu_object_array *objs,
				struct dma_fence *fence);
void virtio_gpu_array_put_free(struct virtio_gpu_object_array *objs);
void virtio_gpu_array_put_free_delayed(struct virtio_gpu_device *vgdev,
				       struct virtio_gpu_object_array *objs);
void virtio_gpu_array_put_free_work(struct work_struct *work);

/* virtgpu_vq.c */
int virtio_gpu_alloc_vbufs(struct virtio_gpu_device *vgdev);
void virtio_gpu_free_vbufs(struct virtio_gpu_device *vgdev);
void virtio_gpu_cmd_create_resource(struct virtio_gpu_device *vgdev,
				    struct virtio_gpu_object *bo,
				    struct virtio_gpu_object_params *params,
				    struct virtio_gpu_object_array *objs,
				    struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_unref_resource(struct virtio_gpu_device *vgdev,
				   struct virtio_gpu_object *bo);
int virtio_gpu_panic_cmd_transfer_to_host_2d(struct virtio_gpu_device *vgdev,
					     uint64_t offset,
					     uint32_t width, uint32_t height,
					     uint32_t x, uint32_t y,
					     struct virtio_gpu_object_array *objs);
void virtio_gpu_cmd_transfer_to_host_2d(struct virtio_gpu_device *vgdev,
					uint64_t offset,
					uint32_t width, uint32_t height,
					uint32_t x, uint32_t y,
					struct virtio_gpu_object_array *objs,
					struct virtio_gpu_fence *fence);
void virtio_gpu_panic_cmd_resource_flush(struct virtio_gpu_device *vgdev,
					 uint32_t resource_id,
					 uint32_t x, uint32_t y,
					 uint32_t width, uint32_t height);
void virtio_gpu_cmd_resource_flush(struct virtio_gpu_device *vgdev,
				   uint32_t resource_id,
				   uint32_t x, uint32_t y,
				   uint32_t width, uint32_t height,
				   struct virtio_gpu_object_array *objs,
				   struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_set_scanout(struct virtio_gpu_device *vgdev,
				uint32_t scanout_id, uint32_t resource_id,
				uint32_t width, uint32_t height,
				uint32_t x, uint32_t y);
void virtio_gpu_object_attach(struct virtio_gpu_device *vgdev,
			      struct virtio_gpu_object *obj,
			      struct virtio_gpu_mem_entry *ents,
			      unsigned int nents);
void virtio_gpu_object_detach(struct virtio_gpu_device *vgdev,
			      struct virtio_gpu_object *obj,
			      struct virtio_gpu_fence *fence);
int virtio_gpu_detach_object_fenced(struct virtio_gpu_object *bo);
void virtio_gpu_cursor_ping(struct virtio_gpu_device *vgdev,
			    struct virtio_gpu_output *output);
int virtio_gpu_cmd_get_display_info(struct virtio_gpu_device *vgdev);
int virtio_gpu_cmd_get_capset_info(struct virtio_gpu_device *vgdev, int idx);
int virtio_gpu_cmd_get_capset(struct virtio_gpu_device *vgdev,
			      int idx, int version,
			      struct virtio_gpu_drv_cap_cache **cache_p);
int virtio_gpu_cmd_get_edids(struct virtio_gpu_device *vgdev);
void virtio_gpu_cmd_context_create(struct virtio_gpu_device *vgdev, uint32_t id,
				   uint32_t context_init, uint32_t nlen,
				   const char *name);
void virtio_gpu_cmd_context_destroy(struct virtio_gpu_device *vgdev,
				    uint32_t id);
void virtio_gpu_cmd_context_attach_resource(struct virtio_gpu_device *vgdev,
					    uint32_t ctx_id,
					    struct virtio_gpu_object_array *objs);
void virtio_gpu_cmd_context_detach_resource(struct virtio_gpu_device *vgdev,
					    uint32_t ctx_id,
					    struct virtio_gpu_object_array *objs);
void virtio_gpu_cmd_submit(struct virtio_gpu_device *vgdev,
			   void *data, uint32_t data_size,
			   uint32_t ctx_id,
			   struct virtio_gpu_object_array *objs,
			   struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_transfer_from_host_3d(struct virtio_gpu_device *vgdev,
					  uint32_t ctx_id,
					  uint64_t offset, uint32_t level,
					  uint32_t stride,
					  uint32_t layer_stride,
					  struct drm_virtgpu_3d_box *box,
					  struct virtio_gpu_object_array *objs,
					  struct virtio_gpu_fence *fence);
void virtio_gpu_cmd_transfer_to_host_3d(struct virtio_gpu_device *vgdev,
					uint32_t ctx_id,
					uint64_t offset, uint32_t level,
					uint32_t stride,
					uint32_t layer_stride,
					struct drm_virtgpu_3d_box *box,
					struct virtio_gpu_object_array *objs,
					struct virtio_gpu_fence *fence);
void
virtio_gpu_cmd_resource_create_3d(struct virtio_gpu_device *vgdev,
				  struct virtio_gpu_object *bo,
				  struct virtio_gpu_object_params *params,
				  struct virtio_gpu_object_array *objs,
				  struct virtio_gpu_fence *fence);
void virtio_gpu_ctrl_ack(struct virtqueue *vq);
void virtio_gpu_cursor_ack(struct virtqueue *vq);
void virtio_gpu_dequeue_ctrl_func(struct work_struct *work);
void virtio_gpu_dequeue_cursor_func(struct work_struct *work);
void virtio_gpu_panic_notify(struct virtio_gpu_device *vgdev);
void virtio_gpu_notify(struct virtio_gpu_device *vgdev);

int
virtio_gpu_cmd_resource_assign_uuid(struct virtio_gpu_device *vgdev,
				    struct virtio_gpu_object_array *objs);

int virtio_gpu_cmd_map(struct virtio_gpu_device *vgdev,
		       struct virtio_gpu_object_array *objs, uint64_t offset);

void virtio_gpu_cmd_unmap(struct virtio_gpu_device *vgdev,
			  struct virtio_gpu_object *bo);

void
virtio_gpu_cmd_resource_create_blob(struct virtio_gpu_device *vgdev,
				    struct virtio_gpu_object *bo,
				    struct virtio_gpu_object_params *params,
				    struct virtio_gpu_mem_entry *ents,
				    uint32_t nents);
void
virtio_gpu_cmd_set_scanout_blob(struct virtio_gpu_device *vgdev,
				uint32_t scanout_id,
				struct virtio_gpu_object *bo,
				struct drm_framebuffer *fb,
				uint32_t width, uint32_t height,
				uint32_t x, uint32_t y);

/* virtgpu_display.c */
int virtio_gpu_modeset_init(struct virtio_gpu_device *vgdev);
void virtio_gpu_modeset_fini(struct virtio_gpu_device *vgdev);

/* virtgpu_plane.c */
uint32_t virtio_gpu_translate_format(uint32_t drm_fourcc);
struct drm_plane *virtio_gpu_plane_init(struct virtio_gpu_device *vgdev,
					enum drm_plane_type type,
					int index);

/* virtgpu_fence.c */
struct virtio_gpu_fence *virtio_gpu_fence_alloc(struct virtio_gpu_device *vgdev,
						uint64_t base_fence_ctx,
						uint32_t ring_idx);
void virtio_gpu_fence_emit(struct virtio_gpu_device *vgdev,
			  struct virtio_gpu_ctrl_hdr *cmd_hdr,
			  struct virtio_gpu_fence *fence);
void virtio_gpu_fence_event_process(struct virtio_gpu_device *vdev,
				    u64 fence_id);

/* virtgpu_object.c */
void virtio_gpu_cleanup_object(struct virtio_gpu_object *bo);
struct drm_gem_object *virtio_gpu_create_object(struct drm_device *dev,
						size_t size);
int virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
			     struct virtio_gpu_object_params *params,
			     struct virtio_gpu_object **bo_ptr,
			     struct virtio_gpu_fence *fence);

bool virtio_gpu_is_shmem(struct virtio_gpu_object *bo);

int virtio_gpu_resource_id_get(struct virtio_gpu_device *vgdev,
			       uint32_t *resid);
/* virtgpu_prime.c */
int virtio_gpu_resource_assign_uuid(struct virtio_gpu_device *vgdev,
				    struct virtio_gpu_object *bo);
struct dma_buf *virtgpu_gem_prime_export(struct drm_gem_object *obj,
					 int flags);
struct drm_gem_object *virtgpu_gem_prime_import(struct drm_device *dev,
						struct dma_buf *buf);
struct drm_gem_object *virtgpu_gem_prime_import_sg_table(
	struct drm_device *dev, struct dma_buf_attachment *attach,
	struct sg_table *sgt);
int virtgpu_dma_buf_import_sgt(struct virtio_gpu_mem_entry **ents,
			       unsigned int *nents,
			       struct virtio_gpu_object *bo,
			       struct dma_buf_attachment *attach);

/* virtgpu_debugfs.c */
void virtio_gpu_debugfs_init(struct drm_minor *minor);

/* virtgpu_vram.c */
bool virtio_gpu_is_vram(struct virtio_gpu_object *bo);
int virtio_gpu_vram_create(struct virtio_gpu_device *vgdev,
			   struct virtio_gpu_object_params *params,
			   struct virtio_gpu_object **bo_ptr);

/* DroidVM guest-alloc pool (gpu_guest), in virtgpu_vram.c */
int virtio_gpu_guest_pool_init(struct virtio_gpu_device *vgdev);
void virtio_gpu_guest_pool_fini(struct virtio_gpu_device *vgdev);
/* Reserve npages contiguous pages; returns byte offset within the pool, or -1 on OOM. */
int virtio_gpu_guest_pool_alloc(struct virtio_gpu_device *vgdev, u64 size,
				struct list_head *blocks);
void virtio_gpu_guest_pool_free(struct virtio_gpu_device *vgdev, struct list_head *blocks);
void virtio_gpu_guest_pool_release_object(struct virtio_gpu_device *vgdev,
					  struct virtio_gpu_object *bo);
void virtio_gpu_guest_pool_stats(struct virtio_gpu_device *vgdev, u64 *total_bytes,
				 u64 *used_bytes, u64 *largest_free_bytes);
int virtio_gpu_guest_pool_create(struct virtio_gpu_device *vgdev,
				 struct virtio_gpu_object_params *params,
				 struct virtio_gpu_object **bo_ptr);
struct sg_table *virtio_gpu_vram_map_dma_buf(struct virtio_gpu_object *bo,
					     struct device *dev,
					     enum dma_data_direction dir);
void virtio_gpu_vram_unmap_dma_buf(struct device *dev,
				   struct sg_table *sgt,
				   enum dma_data_direction dir);

/* virtgpu_submit.c */
int virtio_gpu_execbuffer_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file);


/* See virtgpu_drv.c: per-operation tracing, off unless droidvm_trace is set. */
extern bool virtio_gpu_droidvm_trace;
#define virtio_gpu_trace(fmt, ...)                     \
	do {                                           \
		if (virtio_gpu_droidvm_trace)          \
			pr_info(fmt, ##__VA_ARGS__);   \
	} while (0)

#endif
