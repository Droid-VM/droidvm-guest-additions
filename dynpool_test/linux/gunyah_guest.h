/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal Gunyah guest-side Resource Manager client: lets a (protected) guest
 * accept a memparcel the host SHARE'd to it at runtime, mapping it into the
 * guest's own stage-2 at a chosen IPA. Used by virtio-gpu to map host-visible
 * blobs (the host cannot push a runtime stage-2 mapping into a protected guest).
 */
#ifndef _LINUX_GUNYAH_GUEST_H
#define _LINUX_GUNYAH_GUEST_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_GUNYAH_GUEST)
/* True once the RM msgq capability ids were discovered (DT node present). */
bool gunyah_guest_available(void);
/*
 * Accept a host-SHARE'd memparcel @handle and map it contiguously at guest
 * physical address @gpa (size @size). Returns 0 on success, negative on error.
 */
int gunyah_guest_mem_accept(u32 handle, u64 gpa, u64 size);
/* Release a previously accepted memparcel @handle. */
int gunyah_guest_mem_release(u32 handle);

/*
 * Growable pool control. A pool is declared to this guest whole -- see its reserved-memory node --
 * but backed only in part at boot; these ask the host to back or unback the rest, a step at a time.
 *
 * All three sleep, and NONE may be called from the thread that services the accept transport: a
 * grow completes only after the host has SHARE'd the memory and driven an ACCEPT back to this
 * module, so calling from that path waits on work it is itself required to do. Consumer drivers
 * (a GPU allocator, say) are already a different context.
 *
 * @pool_id indexes the growable pools in guest-physical-address order, matching the order of
 * their device tree nodes. @offset is from the pool base; @offset and @len must both be multiples
 * of the pool's step.
 *
 * NOTHING may touch an address in the growable part before a grow covering it has returned 0.
 * There is no recoverable fault to fall back on: measured on this hardware, a read of an ungranted
 * address returns zeros with no error at all, and a write kills the VM.
 */
int gunyah_pool_grow(u32 pool_id, u64 offset, u64 len);
/*
 * Hand a range back. The HOST decides whether that is safe -- it is the only side that knows
 * whether a dma-buf or GPU mapping still references those pages, because this guest's
 * RESOURCE_UNREF is fire-and-forget. Refusal comes back as an errno, not as a crash.
 */
int gunyah_pool_shrink(u32 pool_id, u64 offset, u64 len);
/*
 * How many grants the host believes are live. For reconciling after a grow returns -ETIMEDOUT --
 * which means the request's fate is UNKNOWN, not that it failed -- or after a driver reload.
 */
int gunyah_pool_query(u32 pool_id, u64 *live_grants);
#else
static inline bool gunyah_guest_available(void) { return false; }
static inline int gunyah_guest_mem_accept(u32 handle, u64 gpa, u64 size) { return -ENODEV; }
static inline int gunyah_guest_mem_release(u32 handle) { return -ENODEV; }
static inline int gunyah_pool_grow(u32 p, u64 o, u64 l) { return -ENODEV; }
static inline int gunyah_pool_shrink(u32 p, u64 o, u64 l) { return -ENODEV; }
static inline int gunyah_pool_query(u32 p, u64 *n) { return -ENODEV; }
#endif

#endif /* _LINUX_GUNYAH_GUEST_H */
