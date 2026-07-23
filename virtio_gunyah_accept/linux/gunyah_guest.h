/* SPDX-License-Identifier: GPL-2.0-only */
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
#else
static inline bool gunyah_guest_available(void) { return false; }
static inline int gunyah_guest_mem_accept(u32 handle, u64 gpa, u64 size) { return -ENODEV; }
static inline int gunyah_guest_mem_release(u32 handle) { return -ENODEV; }
#endif

#endif /* _LINUX_GUNYAH_GUEST_H */
