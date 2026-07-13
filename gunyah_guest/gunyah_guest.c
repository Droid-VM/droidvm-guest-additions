// SPDX-License-Identifier: GPL-2.0-only
/*
 * Gunyah guest-side Resource Manager client.
 *
 * A (protected) guest cannot receive a runtime stage-2 mapping pushed by the
 * host: the guest owns its own stage-2. Instead the host SHARE's a memparcel and
 * the guest ACCEPTs it here, mapping it into its own stage-2 at a chosen IPA.
 *
 * The RM msgq is driven with raw HVC hypercalls (no Gunyah driver dependency);
 * the tx/rx capability ids come from the RM-generated "gunyah-resource-manager"
 * DT node (reg = <tx_capid rx_capid>). Validated end-to-end: a SHARE'd parcel is
 * accepted with no ACL (RM uses its stored copy) and the host data is visible.
 */

#define pr_fmt(fmt) "gunyah_guest: " fmt

#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/gunyah_guest.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/types.h>
#include <linux/unaligned.h>

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

static int __init gunyah_guest_init(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "gunyah-resource-manager");
	if (!np) {
		pr_info("no \"gunyah-resource-manager\" DT node; RM client disabled\n");
		return 0;
	}
	if (of_property_read_u64_index(np, "reg", 0, &gg_tx_capid) ||
	    of_property_read_u64_index(np, "reg", 1, &gg_rx_capid)) {
		pr_info("RM node present but reg=<tx rx> missing\n");
		of_node_put(np);
		return 0;
	}
	of_node_put(np);

	WRITE_ONCE(gg_ready, true);
	pr_info("RM client ready (tx_capid=%llu rx_capid=%llu)\n",
		gg_tx_capid, gg_rx_capid);
	return 0;
}
core_initcall(gunyah_guest_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah guest RM client (raw HVC mem_accept/release) for GuestAccept blobs");
