// SPDX-License-Identifier: GPL-2.0-only
/*
 * virtio-gunyah-accept: host->guest transport for runtime memparcel accept
 * (crosvm VmAccept::Sync).
 *
 * The host (crosvm vm_control) runtime-SHAREs a memparcel to this protected
 * guest and asks THIS driver -- over a dedicated virtio device -- to accept
 * it at the given GPA (and symmetrically release it before the host
 * unshares). That lets any crosvm component use runtime memory attach with
 * plain upstream add/remove semantics; only virtio-gpu keeps driving its own
 * accepts (the VmAccept::Off fast path in virtgpu_vram.c).
 *
 * Queue 0 (requestq):    driver posts device-writable buffers; the device
 *                        fills one per request {req_id, op, handle, gpa, size}.
 * Queue 1 (completionq): driver posts device-readable completions
 *                        {req_id, ret} once the RM RPC finished.
 *
 * The RM accept core is gunyah_guest.ko (raw-HVC client); this driver is its
 * second in-kernel consumer, alongside the virtio-gpu front. Accepted parcels
 * are tracked gpa-keyed HERE: the guest owns the handle for its stage-2
 * acceptance, the host stays stateless (labels derived from gpa>>12), so a
 * RELEASE request carries only the gpa.
 */

#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/gunyah_guest.h>

/* Non-standard virtio device id, counting down from 63 like VIRTIO_ID_WL. */
#define VIRTIO_ID_GUNYAH_ACCEPT 60

#define VGA_OP_ACCEPT  1
#define VGA_OP_RELEASE 2

#define VGA_NUM_REQ_BUFS 16

struct virtio_gunyah_accept_req {
	__le32 req_id;
	__le32 op;
	__le32 handle;	/* ACCEPT: RM memparcel handle; RELEASE: unused (gpa-keyed) */
	__le32 flags;	/* reserved, 0 */
	__le64 gpa;
	__le64 size;	/* ACCEPT only */
};

struct virtio_gunyah_accept_comp {
	__le32 req_id;
	__le32 ret;	/* 0 or negative errno, two's complement */
};

/* gpa-keyed owner table entry: the guest side of a Sync-accepted parcel. */
struct vga_accepted {
	struct list_head node;
	u64 gpa;
	u64 size;
	u32 handle;
};

struct vga_dev {
	struct virtio_device *vdev;
	struct virtqueue *req_vq;
	struct virtqueue *comp_vq;
	/* Serializes comp_vq access between the work fn and the reclaim cb. */
	spinlock_t comp_lock;
	struct work_struct work;
	struct list_head accepted;	/* struct vga_accepted, work-ctx only */
	struct virtio_gunyah_accept_req *req_bufs[VGA_NUM_REQ_BUFS];
};

static int vga_do_accept(struct vga_dev *vga, u32 handle, u64 gpa, u64 size)
{
	struct vga_accepted *ent;
	int ret;

	if (!gunyah_guest_available())
		return -ENODEV;
	if (!handle || !size)
		return -EINVAL;

	ent = kzalloc(sizeof(*ent), GFP_KERNEL);
	if (!ent)
		return -ENOMEM;

	ret = gunyah_guest_mem_accept(handle, gpa, size);
	if (ret) {
		kfree(ent);
		return ret;
	}

	ent->gpa = gpa;
	ent->size = size;
	ent->handle = handle;
	list_add_tail(&ent->node, &vga->accepted);
	return 0;
}

static int vga_do_release(struct vga_dev *vga, u64 gpa)
{
	struct vga_accepted *ent;
	int ret;

	list_for_each_entry(ent, &vga->accepted, node) {
		if (ent->gpa == gpa) {
			/* Release BEFORE the host unshares/reuses the gpa
			 * (same ordering invariant as virtgpu_vram.c). */
			ret = gunyah_guest_mem_release(ent->handle);
			list_del(&ent->node);
			kfree(ent);
			return ret;
		}
	}
	return -ENOENT;
}

static void vga_send_completion(struct vga_dev *vga, u32 req_id, int ret)
{
	struct virtio_gunyah_accept_comp *comp;
	struct scatterlist sg;
	unsigned long flags;

	comp = kzalloc(sizeof(*comp), GFP_KERNEL);
	if (!comp)
		return;	/* host times out; nothing better to do */
	comp->req_id = cpu_to_le32(req_id);
	comp->ret = cpu_to_le32((u32)ret);
	sg_init_one(&sg, comp, sizeof(*comp));

	spin_lock_irqsave(&vga->comp_lock, flags);
	if (virtqueue_add_outbuf(vga->comp_vq, &sg, 1, comp, GFP_ATOMIC))
		kfree(comp);
	else
		virtqueue_kick(vga->comp_vq);
	spin_unlock_irqrestore(&vga->comp_lock, flags);
}

static void vga_work_func(struct work_struct *work)
{
	struct vga_dev *vga = container_of(work, struct vga_dev, work);
	struct virtio_gunyah_accept_req *req;
	unsigned int len;

	while ((req = virtqueue_get_buf(vga->req_vq, &len))) {
		u32 req_id = le32_to_cpu(req->req_id);
		u32 op = le32_to_cpu(req->op);
		int ret;

		if (len < sizeof(*req)) {
			ret = -EINVAL;
		} else {
			switch (op) {
			case VGA_OP_ACCEPT:
				ret = vga_do_accept(vga,
						    le32_to_cpu(req->handle),
						    le64_to_cpu(req->gpa),
						    le64_to_cpu(req->size));
				break;
			case VGA_OP_RELEASE:
				ret = vga_do_release(vga,
						     le64_to_cpu(req->gpa));
				break;
			default:
				ret = -EINVAL;
			}
		}
		if (ret)
			dev_warn(&vga->vdev->dev,
				 "req %u op %u gpa %#llx failed: %d\n",
				 req_id, op, le64_to_cpu(req->gpa), ret);

		vga_send_completion(vga, req_id, ret);

		/* Recycle the request buffer back to the device. */
		{
			struct scatterlist sg;

			sg_init_one(&sg, req, sizeof(*req));
			if (virtqueue_add_inbuf(vga->req_vq, &sg, 1, req,
						GFP_KERNEL))
				dev_err(&vga->vdev->dev,
					"failed to recycle request buffer\n");
			else
				virtqueue_kick(vga->req_vq);
		}
	}
}

static void vga_req_cb(struct virtqueue *vq)
{
	struct vga_dev *vga = vq->vdev->priv;

	/* The RM RPC sleeps; punt to process context. */
	schedule_work(&vga->work);
}

static void vga_comp_cb(struct virtqueue *vq)
{
	struct vga_dev *vga = vq->vdev->priv;
	struct virtio_gunyah_accept_comp *comp;
	unsigned int len;
	unsigned long flags;

	/* Host consumed a completion; reclaim the buffer. */
	spin_lock_irqsave(&vga->comp_lock, flags);
	while ((comp = virtqueue_get_buf(vq, &len)))
		kfree(comp);
	spin_unlock_irqrestore(&vga->comp_lock, flags);
}

static int vga_probe(struct virtio_device *vdev)
{
	struct virtqueue_info vqs_info[] = {
		{ "request", vga_req_cb },
		{ "completion", vga_comp_cb },
	};
	struct virtqueue *vqs[2];
	struct vga_dev *vga;
	int i, ret;

	vga = kzalloc(sizeof(*vga), GFP_KERNEL);
	if (!vga)
		return -ENOMEM;
	vga->vdev = vdev;
	vdev->priv = vga;
	spin_lock_init(&vga->comp_lock);
	INIT_LIST_HEAD(&vga->accepted);
	INIT_WORK(&vga->work, vga_work_func);

	ret = virtio_find_vqs(vdev, 2, vqs, vqs_info, NULL);
	if (ret)
		goto err_free;
	vga->req_vq = vqs[0];
	vga->comp_vq = vqs[1];

	/* Pre-post the device-writable request buffers. */
	for (i = 0; i < VGA_NUM_REQ_BUFS; i++) {
		struct scatterlist sg;

		vga->req_bufs[i] = kzalloc(sizeof(*vga->req_bufs[i]),
					   GFP_KERNEL);
		if (!vga->req_bufs[i]) {
			ret = -ENOMEM;
			goto err_bufs;
		}
		sg_init_one(&sg, vga->req_bufs[i], sizeof(*vga->req_bufs[i]));
		ret = virtqueue_add_inbuf(vga->req_vq, &sg, 1,
					  vga->req_bufs[i], GFP_KERNEL);
		if (ret)
			goto err_bufs;
	}

	virtio_device_ready(vdev);
	virtqueue_kick(vga->req_vq);

	dev_info(&vdev->dev, "gunyah accept transport ready (rm %savailable)\n",
		 gunyah_guest_available() ? "" : "NOT ");
	return 0;

err_bufs:
	for (i = 0; i < VGA_NUM_REQ_BUFS; i++)
		kfree(vga->req_bufs[i]);
	vdev->config->del_vqs(vdev);
err_free:
	kfree(vga);
	return ret;
}

static void vga_remove(struct virtio_device *vdev)
{
	struct vga_dev *vga = vdev->priv;
	struct vga_accepted *ent, *tmp;
	int i;

	virtio_reset_device(vdev);
	cancel_work_sync(&vga->work);
	vdev->config->del_vqs(vdev);

	/* Drop any still-accepted parcels (host reclaims after us; errors
	 * here are harmless at teardown). */
	list_for_each_entry_safe(ent, tmp, &vga->accepted, node) {
		gunyah_guest_mem_release(ent->handle);
		list_del(&ent->node);
		kfree(ent);
	}

	for (i = 0; i < VGA_NUM_REQ_BUFS; i++)
		kfree(vga->req_bufs[i]);
	kfree(vga);
}

static const struct virtio_device_id vga_id_table[] = {
	{ VIRTIO_ID_GUNYAH_ACCEPT, VIRTIO_DEV_ANY_ID },
	{ 0 },
};
MODULE_DEVICE_TABLE(virtio, vga_id_table);

static struct virtio_driver vga_driver = {
	.driver.name = KBUILD_MODNAME,
	.id_table = vga_id_table,
	.probe = vga_probe,
	.remove = vga_remove,
};
module_virtio_driver(vga_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("virtio transport for Gunyah runtime memparcel accept (VmAccept::Sync)");
