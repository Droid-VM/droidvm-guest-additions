// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gunyah guest-side Resource Manager client + runtime memparcel accept transport.
 *
 * Part 1 -- RM client:
 *   A (protected) guest cannot receive a runtime stage-2 mapping pushed by the
 *   host: the guest owns its own stage-2. Instead the host SHARE's a memparcel and
 *   the guest ACCEPTs it here, mapping it into its own stage-2 at a chosen IPA.
 *   The RM msgq is driven with raw HVC hypercalls (no Gunyah driver dependency);
 *   the tx/rx capability ids come from the RM-generated "gunyah-resource-manager"
 *   DT node (reg = <tx_capid rx_capid>). The accept/release primitives are
 *   EXPORT_SYMBOL_GPL'd for in-kernel consumers -- notably virtio-gpu's
 *   VmAccept::Off padding path, which drives its own accepts.
 *
 * Part 2 -- virtio accept device (crosvm VmAccept::Sync):
 *   The host (crosvm vm_control) runtime-SHAREs a memparcel to this protected
 *   guest and asks THIS driver -- over a dedicated virtio device -- to accept it
 *   at the given GPA (and symmetrically release it before the host unshares).
 *   That lets any crosvm component use runtime memory attach with plain upstream
 *   add/remove semantics.
 *     Queue 0 (requestq):    driver posts device-writable buffers; the device
 *                            fills one per request {req_id, op, handle, gpa, size}.
 *     Queue 1 (completionq): driver posts device-readable completions
 *                            {req_id, ret} once the RM RPC finished.
 *   Accepted parcels are tracked gpa-keyed HERE: the guest owns the handle for
 *   its stage-2 acceptance, the host stays stateless (labels derived from
 *   gpa>>12), so a RELEASE request carries only the gpa.
 *
 * Why one module: the RM client's EXPORT_SYMBOL_GPL symbols are published at
 * module load, independent of any device. So virtio-gpu keeps resolving them
 * whether or not the Sync accept device is present. When there is no accept
 * device (VmAccept::Off), register_virtio_driver() still succeeds and the driver
 * simply sits idle -- .probe never fires -- while the exported API stays live.
 */

#define pr_fmt(fmt) "gunyah_guest: " fmt

#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/gunyah_guest.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/workqueue.h>

/* ======================================================================
 * Part 1: RM client (raw-HVC msgq)
 * ====================================================================== */

/* Gunyah hypercall IDs: vendor-hyp fast SMC64 calls. */
#define GH_HCALL(fn)							\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,	\
			   ARM_SMCCC_OWNER_VENDOR_HYP, (fn))
#define GH_HCALL_MSGQ_SEND	GH_HCALL(0x801B)
#define GH_HCALL_MSGQ_RECV	GH_HCALL(0x801C)
#define GH_MSGQ_TX_PUSH		BIT(0)
#define GH_ERROR_OK		0

/* RM RPC framing. */
#define GH_RM_RPC_API		0x21	/* version 1 | hdr_words 2 (<<4) */
#define GH_RM_RPC_TYPE_REQUEST	0x01
#define GH_RM_RPC_TYPE_REPLY	0x02
#define GH_RM_RPC_TYPE_MASK	0x03

#define GH_RM_RPC_MEM_ACCEPT	0x51000011
#define GH_RM_RPC_MEM_RELEASE	0x51000014

#define GH_RM_MSGQ_MSG_SIZE	240

/* MEM_ACCEPT constants (production form proven in bring-up). */
#define GH_RM_MEM_TYPE_NORMAL	0
#define GH_RM_TRANS_TYPE_SHARE	2
#define GH_RM_MEM_ACCEPT_MAP_IPA_CONTIGUOUS	BIT(4)
#define GH_RM_MEM_ACCEPT_DONE			BIT(7)

struct gh_rpc_reply {
	u8 api;
	u8 type;
	__le16 seq;
	__le32 msg_id;
	__le32 err_code;
} __packed;

static u64 gg_tx_capid, gg_rx_capid;
static bool gg_ready;
static u16 gg_seq = 1;
static DEFINE_MUTEX(gg_lock);
static u8 gg_txbuf[64] __aligned(8);
static u8 gg_rxbuf[GH_RM_MSGQ_MSG_SIZE] __aligned(8);

static int gg_msgq_send(u64 capid, void *buff, size_t size)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GH_HCALL_MSGQ_SEND, capid, size, (uintptr_t)buff,
			  GH_MSGQ_TX_PUSH, 0, &res);
	return (int)res.a0;
}

static int gg_msgq_recv(u64 capid, void *buff, size_t size, size_t *recv_size)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GH_HCALL_MSGQ_RECV, capid, (uintptr_t)buff, size,
			  0, &res);
	if (res.a0 == GH_ERROR_OK)
		*recv_size = res.a1;
	return (int)res.a0;
}

/* Send @txlen bytes, poll for the REPLY matching @seq, return RM err_code. */
static int gg_rpc(size_t txlen, u16 seq, u32 *err_code)
{
	struct gh_rpc_reply *r = (void *)gg_rxbuf;
	size_t rxlen = 0;
	int err, i;

	err = gg_msgq_send(gg_tx_capid, gg_txbuf, txlen);
	if (err) {
		pr_err("msgq_send failed: gunyah_error=%d\n", err);
		return -EIO;
	}

	for (i = 0; i < 2000; i++) {
		rxlen = 0;
		err = gg_msgq_recv(gg_rx_capid, gg_rxbuf, sizeof(gg_rxbuf),
				   &rxlen);
		if (err == GH_ERROR_OK && rxlen >= sizeof(*r) &&
		    (r->type & GH_RM_RPC_TYPE_MASK) == GH_RM_RPC_TYPE_REPLY &&
		    le16_to_cpu(r->seq) == seq) {
			*err_code = le32_to_cpu(r->err_code);
			return 0;
		}
		udelay(100);
	}
	pr_err("RPC timeout (last gunyah_error=%d)\n", err);
	return -ETIMEDOUT;
}

bool gunyah_guest_available(void)
{
	return READ_ONCE(gg_ready);
}
EXPORT_SYMBOL_GPL(gunyah_guest_available);

int gunyah_guest_mem_accept(u32 handle, u64 gpa, u64 size)
{
	u8 *p = gg_txbuf;
	size_t off = 0;
	u32 err_code = 0;
	u16 seq;
	int ret;

	if (!READ_ONCE(gg_ready))
		return -ENODEV;

	mutex_lock(&gg_lock);
	seq = ++gg_seq;

	/* RPC header (8) */
	p[off++] = GH_RM_RPC_API;
	p[off++] = GH_RM_RPC_TYPE_REQUEST;
	put_unaligned_le16(seq, p + off); off += 2;
	put_unaligned_le32(GH_RM_RPC_MEM_ACCEPT, p + off); off += 4;
	/* accept hdr (12) */
	put_unaligned_le32(handle, p + off); off += 4;
	p[off++] = GH_RM_MEM_TYPE_NORMAL;
	p[off++] = GH_RM_TRANS_TYPE_SHARE;
	/*
	 * MAP_IPA_CONTIGUOUS + a SINGLE sgl {gpa, size} is the correct form for a
	 * scatter-gather blob. A >2MB blob is backed by several independent, non-
	 * adjacent 2MB folios, so the memparcel carries N mem_entries (one per
	 * physically-contiguous run). The RM's accept has exactly two shapes
	 * (rsc-mgr memparcel_do_accept): num_mappings = contiguous ? 1 : N. With the
	 * flag OFF the guest must supply N sgl entries, each sized to a region -- a
	 * layout the guest cannot know. With the flag ON, num_mappings=1: the guest
	 * gives ONE sgl {gpa, total_size} (total_size must equal mp->total_size, i.e.
	 * the exact page-aligned byte count the host share_blob()'d = obj->size) and
	 * the RM allocates one contiguous IPA range and lays the N scattered
	 * mem_entries into it sequentially -- IPA contiguous, PA scattered. A single
	 * sgl WITHOUT the flag gave err_code=0x6 (ARGUMENT_INVALID: 1 != N mappings).
	 */
	p[off++] = GH_RM_MEM_ACCEPT_MAP_IPA_CONTIGUOUS | GH_RM_MEM_ACCEPT_DONE;
	p[off++] = 0;						/* reserved1 */
	put_unaligned_le32(0, p + off); off += 4;		/* validate_label */
	/* acl_desc: n=0 (SHARE accepted with no ACL; RM uses its stored copy) */
	put_unaligned_le32(0, p + off); off += 4;
	/* sgl_desc_intf: n(u16)=1, reserved(u16=map_vmid 0), one {ipa,size} */
	put_unaligned_le16(1, p + off); off += 2;
	put_unaligned_le16(0, p + off); off += 2;
	put_unaligned_le64(gpa, p + off); off += 8;
	put_unaligned_le64(size, p + off); off += 8;
	/* mem_attr_desc: n=0 */
	put_unaligned_le16(0, p + off); off += 2;
	put_unaligned_le16(0, p + off); off += 2;

	ret = gg_rpc(off, seq, &err_code);
	mutex_unlock(&gg_lock);

	if (ret)
		return ret;
	if (err_code) {
		pr_err("MEM_ACCEPT handle=0x%x gpa=0x%llx rejected, err_code=0x%x\n",
		       handle, gpa, err_code);
		return -EACCES;
	}
	pr_debug("MEM_ACCEPT handle=0x%x mapped at gpa=0x%llx size=0x%llx\n",
		 handle, gpa, size);
	return 0;
}
EXPORT_SYMBOL_GPL(gunyah_guest_mem_accept);

int gunyah_guest_mem_release(u32 handle)
{
	u8 *p = gg_txbuf;
	size_t off = 0;
	u32 err_code = 0;
	u16 seq;
	int ret;

	if (!READ_ONCE(gg_ready))
		return -ENODEV;

	mutex_lock(&gg_lock);
	seq = ++gg_seq;

	/* RPC header (8) */
	p[off++] = GH_RM_RPC_API;
	p[off++] = GH_RM_RPC_TYPE_REQUEST;
	put_unaligned_le16(seq, p + off); off += 2;
	put_unaligned_le32(GH_RM_RPC_MEM_RELEASE, p + off); off += 4;
	/* release payload: u32 handle, u8 flags, u24 reserved */
	put_unaligned_le32(handle, p + off); off += 4;
	put_unaligned_le32(0, p + off); off += 4;	/* flags=0 | reserved */

	ret = gg_rpc(off, seq, &err_code);
	mutex_unlock(&gg_lock);

	if (ret)
		return ret;
	if (err_code) {
		pr_err("MEM_RELEASE handle=0x%x err_code=0x%x\n", handle, err_code);
		return -EIO;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(gunyah_guest_mem_release);

/* ======================================================================
 * Part 2: virtio accept device (VmAccept::Sync)
 * ====================================================================== */

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

/*
 * Queue 2, guest -> host: the pool control plane.
 *
 * A third queue rather than a new op on the pair above, for two reasons visible in the structs:
 * the completion is eight bytes with nowhere to put an offset and a length, and its req_id is
 * assigned by the HOST, which drops any completion whose id it did not issue. Carrying the
 * direction in the queue also means the host cannot forget to range-check -- everything arriving
 * there is by construction guest-originated.
 */
#define VGP_OP_SHARE	1
#define VGP_OP_UNSHARE	2
#define VGP_OP_QUERY	3

struct virtio_gunyah_pool_req {
	__le32 req_id;		/* guest-assigned; echoed back in the response */
	__le32 op;
	__le32 pool_id;		/* index of the growable pool, in address order */
	__le32 flags;		/* reserved, 0 */
	__le64 offset;		/* from the pool base */
	__le64 len;
};

struct virtio_gunyah_pool_resp {
	__le32 req_id;
	__le32 ret;		/* 0 or negative errno */
	__le64 extra;		/* QUERY: live grant count. Otherwise 0. */
};

/* One in-flight pool request. The submitter sleeps on `done`; the vq callback wakes it. */
struct vga_pool_ctx {
	struct virtio_gunyah_pool_req req;
	struct virtio_gunyah_pool_resp resp;
	struct completion done;
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
	struct virtqueue *pool_vq;
	/* Serializes pool_vq submission; the queue is shallow and requests are rare. */
	struct mutex pool_lock;
	spinlock_t pool_cb_lock;
	u32 pool_next_id;
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

/* Pool response arrived: wake whoever is waiting on it. */
static void vga_pool_cb(struct virtqueue *vq)
{
	struct vga_dev *vga = vq->vdev->priv;
	struct vga_pool_ctx *ctx;
	unsigned long flags;
	unsigned int len;

	spin_lock_irqsave(&vga->pool_cb_lock, flags);
	while ((ctx = virtqueue_get_buf(vq, &len)))
		complete(&ctx->done);
	spin_unlock_irqrestore(&vga->pool_cb_lock, flags);
}

/* The one device, for the exported API. Set at probe, cleared at remove. */
static struct vga_dev *vga_singleton;
static DEFINE_MUTEX(vga_singleton_lock);

/*
 * Ask the host to grow or shrink a growable pool, and wait for the answer.
 *
 * MUST be called from process context and MUST NOT be called from the thread that services the
 * accept requestq: a grow is answered only after the host has SHARE'd the memory and driven an
 * ACCEPT back through queue 0, so a caller that is itself the accept path would be waiting on
 * work it is required to perform. In practice callers are consumer drivers (the GPU allocator,
 * say), which are already a different context; the rule is written down because violating it
 * deadlocks rather than failing.
 *
 * Returns the host's verdict: 0, or a negative errno. -ETIMEDOUT means the request's fate is
 * UNKNOWN -- the host may have completed it -- so the caller must reconcile with
 * gunyah_pool_query() rather than assume it did not happen.
 */
static int vga_pool_request(u32 op, u32 pool_id, u64 offset, u64 len, u64 *extra)
{
	struct scatterlist sg_out, sg_in, *sgs[2];
	struct vga_pool_ctx *ctx;
	struct vga_dev *vga;
	int ret;

	might_sleep();

	mutex_lock(&vga_singleton_lock);
	vga = vga_singleton;
	mutex_unlock(&vga_singleton_lock);
	if (!vga || !vga->pool_vq)
		return -ENODEV;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	init_completion(&ctx->done);

	mutex_lock(&vga->pool_lock);
	ctx->req.req_id  = cpu_to_le32(vga->pool_next_id++);
	ctx->req.op      = cpu_to_le32(op);
	ctx->req.pool_id = cpu_to_le32(pool_id);
	ctx->req.offset  = cpu_to_le64(offset);
	ctx->req.len     = cpu_to_le64(len);

	sg_init_one(&sg_out, &ctx->req, sizeof(ctx->req));
	sg_init_one(&sg_in, &ctx->resp, sizeof(ctx->resp));
	sgs[0] = &sg_out;
	sgs[1] = &sg_in;
	ret = virtqueue_add_sgs(vga->pool_vq, sgs, 1, 1, ctx, GFP_KERNEL);
	if (ret) {
		mutex_unlock(&vga->pool_lock);
		kfree(ctx);
		return ret;
	}
	virtqueue_kick(vga->pool_vq);
	mutex_unlock(&vga->pool_lock);

	/*
	 * Generous, because a grow is not one round trip: the host allocates the backing, SHAREs
	 * it, then drives an ACCEPT back to this module and waits for the RM. Short timeouts here
	 * would turn a slow grow into a desync, which is far more expensive than waiting.
	 */
	if (!wait_for_completion_timeout(&ctx->done, msecs_to_jiffies(30000))) {
		/*
		 * The buffer is still owned by the device, so it cannot be freed -- leak the ctx
		 * rather than hand the host memory that may be reused. Rare enough to be the right
		 * trade; a device reset reclaims it.
		 */
		dev_err(&vga->vdev->dev,
			"pool op=%u pool=%u offset=%#llx len=%#llx timed out; state unknown\n",
			op, pool_id, offset, len);
		return -ETIMEDOUT;
	}

	ret = (s32)le32_to_cpu(ctx->resp.ret);
	if (extra)
		*extra = le64_to_cpu(ctx->resp.extra);
	kfree(ctx);
	return ret;
}

/* Grow a pool by [offset, offset+len). Both must be multiples of the pool's step. */
int gunyah_pool_grow(u32 pool_id, u64 offset, u64 len)
{
	return vga_pool_request(VGP_OP_SHARE, pool_id, offset, len, NULL);
}
EXPORT_SYMBOL_GPL(gunyah_pool_grow);

/* Hand a range back. The HOST decides whether it is safe: it is the only side that knows whether
 * a dma-buf or GPU mapping still references those pages, because this guest's RESOURCE_UNREF is
 * fire-and-forget. */
int gunyah_pool_shrink(u32 pool_id, u64 offset, u64 len)
{
	return vga_pool_request(VGP_OP_UNSHARE, pool_id, offset, len, NULL);
}
EXPORT_SYMBOL_GPL(gunyah_pool_shrink);

/* How many grants the HOST believes are live. For reconciling after a timeout or a driver reload,
 * where this side's idea of what is granted may be wrong. */
int gunyah_pool_query(u32 pool_id, u64 *live_grants)
{
	return vga_pool_request(VGP_OP_QUERY, pool_id, 0, 0, live_grants);
}
EXPORT_SYMBOL_GPL(gunyah_pool_query);

static int vga_probe(struct virtio_device *vdev)
{
	struct virtqueue_info vqs_info[] = {
		{ "request", vga_req_cb },
		{ "completion", vga_comp_cb },
		{ "pool", vga_pool_cb },
	};
	struct virtqueue *vqs[3];
	struct vga_dev *vga;
	int i, ret;

	vga = kzalloc(sizeof(*vga), GFP_KERNEL);
	if (!vga)
		return -ENOMEM;
	vga->vdev = vdev;
	vdev->priv = vga;
	spin_lock_init(&vga->comp_lock);
	spin_lock_init(&vga->pool_cb_lock);
	mutex_init(&vga->pool_lock);
	vga->pool_next_id = 1;
	INIT_LIST_HEAD(&vga->accepted);
	INIT_WORK(&vga->work, vga_work_func);

	ret = virtio_find_vqs(vdev, 3, vqs, vqs_info, NULL);
	if (ret)
		goto err_free;
	vga->req_vq = vqs[0];
	vga->comp_vq = vqs[1];
	vga->pool_vq = vqs[2];

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

	mutex_lock(&vga_singleton_lock);
	vga_singleton = vga;
	mutex_unlock(&vga_singleton_lock);

	dev_info(&vdev->dev, "gunyah accept transport ready (rm %savailable), pool queue up\n",
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

	/* Stop new pool requests finding the device before anything is torn down. A caller already
	 * inside vga_pool_request holds no reference to it beyond the vq, which virtio_reset_device
	 * below completes -- its wait then times out and reports the state as unknown, which is the
	 * honest answer during an unbind. */
	mutex_lock(&vga_singleton_lock);
	if (vga_singleton == vga)
		vga_singleton = NULL;
	mutex_unlock(&vga_singleton_lock);

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
	.driver.name = "gunyah_accept",
	.id_table = vga_id_table,
	.probe = vga_probe,
	.remove = vga_remove,
};
/* Registered from gunyah_guest_init() below -- NOT via module_virtio_driver(),
 * so the RM exports above stay live even when no accept device is present. */

/* ======================================================================
 * Module init / exit
 * ====================================================================== */

static int __init gunyah_guest_init(void)
{
	struct device_node *np;
	int ret;

	/* Part 1: bring up the RM client (never fatal -- a guest with no RM node
	 * just leaves gunyah_guest_available() false). */
	np = of_find_compatible_node(NULL, NULL, "gunyah-resource-manager");
	if (!np) {
		pr_info("no \"gunyah-resource-manager\" DT node; RM client disabled\n");
	} else if (of_property_read_u64_index(np, "reg", 0, &gg_tx_capid) ||
		   of_property_read_u64_index(np, "reg", 1, &gg_rx_capid)) {
		pr_info("RM node present but reg=<tx rx> missing\n");
		of_node_put(np);
	} else {
		of_node_put(np);
		WRITE_ONCE(gg_ready, true);
		pr_info("RM client ready (tx_capid=%llu rx_capid=%llu)\n",
			gg_tx_capid, gg_rx_capid);
	}

	/* Part 2: register the Sync accept transport. This never fails for lack
	 * of a device (no matching virtio device -> .probe simply never fires),
	 * and even if registration itself failed the exported RM API above must
	 * stay live for virtio-gpu, so we log and still return success. */
	ret = register_virtio_driver(&vga_driver);
	if (ret)
		pr_warn("accept transport register failed (%d); RM API still exported\n",
			ret);
	return 0;
}
module_init(gunyah_guest_init);

static void __exit gunyah_guest_exit(void)
{
	unregister_virtio_driver(&vga_driver);
}
module_exit(gunyah_guest_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah guest RM client + runtime memparcel accept transport (VmAccept::Sync)");
