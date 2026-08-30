// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright DroidVM contributors
// Additional permissions apply; see ADDITIONAL-PERMISSIONS in the repository root.

/*
 * Exercise a growable pool from inside the guest.
 *
 * The pool is declared to us whole in a `droidvm,dynamic-pool` reserved-memory node but backed
 * only up to `droidvm,pre-alloc-size`; the rest arrives when we ask for it, a `droidvm,step-size`
 * multiple at a time, over virtio-gunyah-accept's pool queue.
 *
 * WHAT MUST BE TRUE AND CANNOT BE CHECKED FROM HERE
 * -------------------------------------------------
 * Touching the growable part before a grow covering it has returned 0 is not a recoverable error
 * and this module cannot protect you from it. Measured on this hardware:
 *
 *     read  an ungranted address -> returns ZEROS. No fault, no error, no log, VM survives.
 *     write an ungranted address -> "page fault ... attempt: -2", the vcpu dies, VM gone.
 *
 * The read is the dangerous direction: it is indistinguishable from real memory that happens to
 * be zero. So this module never touches an address it has not just been granted, and the negative
 * tests below deliberately exercise REQUEST rejection rather than access faults -- asking for a
 * misaligned range is safe and proves the host's validation; reading an ungranted address proves
 * nothing and corrupts the result of whatever runs next.
 *
 * WHAT THE HOST SIDE SHOULD SHOW
 * ------------------------------
 * A grow is not just bookkeeping: the range is faulted in and folded into 2 MiB folios, which the
 * reserve module's allocation hook intercepts. So on the Android side, across a grow:
 *
 *     /sys/module/gh_hugepage_reserve/parameters/refill_stat: served RISES, pool_avail FALLS
 *
 * and across a shrink the reverse, because the host punches the range out of the pool memfd and
 * the module's order-9 free hook takes the pages back. If served does not move, the grant did not
 * come from the reserve pool and something is wrong even though every test here passes.
 *
 *   echo "grow <offset_mb> <len_mb>"  > /sys/kernel/dynpool_test/cmd
 *   echo "shrink <offset_mb> <len_mb>" > /sys/kernel/dynpool_test/cmd
 *   echo "verify <offset_mb> <len_mb>" > /sys/kernel/dynpool_test/cmd   (write+read a pattern)
 *   echo "reject" > /sys/kernel/dynpool_test/cmd    (every rejection path, no memory touched)
 *   echo "selftest" > /sys/kernel/dynpool_test/cmd  (grow, verify, query, shrink)
 *   cat /sys/kernel/dynpool_test/result
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <asm/cacheflush.h>
#include <linux/gunyah_guest.h>

#define MB (1UL << 20)

static u64 pool_base, pool_size, pool_prealloc, pool_step;
static u32 pool_id;
static struct kobject *dp_kobj;
static bool dp_misc_ok;
/* What /dev/dynpool maps: the active pool, or the range `accept` just took (which is outside
 * every pool, and is the case the pseudo-unprotected window cares about). */
static u64 dp_map_base, dp_map_size;

/* All droidvm,dynamic-pool nodes, so the two-pool / hole-in-the-middle case can be
 * driven from one module. `select <pool_id>` repoints the globals above at one of these. */
#define DP_MAX_POOLS 4
struct dp_pool {
	u64 base, size, prealloc, step;
	u32 id;
};
static struct dp_pool dp_pools[DP_MAX_POOLS];
static int dp_npools;

/*
 * Module parameters that stand in for the device-tree node.
 *
 * The sm8650-era RM refuses to start a VM whose `/reserved-memory` child has a `reg` that no
 * accepted memparcel matches -- which is every pool that is declared but not pre-shared, i.e.
 * exactly the growable ones. crosvm can leave the node out (DROIDVM_POOL_HIDE=dt) and the pool
 * still works, because the pool table lives on the host and is keyed by pool-id, not by the node.
 * The guest then has nothing to read, so it is told here instead.
 */
static u64 param_base, param_size, param_prealloc, param_step;
static uint param_id;
module_param_named(base, param_base, ullong, 0444);
MODULE_PARM_DESC(base, "pool base GPA when the DT node is absent");
module_param_named(size, param_size, ullong, 0444);
module_param_named(prealloc, param_prealloc, ullong, 0444);
module_param_named(step, param_step, ullong, 0444);
module_param_named(id, param_id, uint, 0444);

static void dp_activate(int idx)
{
	if (idx < 0 || idx >= dp_npools)
		return;
	pool_base = dp_pools[idx].base;
	pool_size = dp_pools[idx].size;
	pool_prealloc = dp_pools[idx].prealloc;
	pool_step = dp_pools[idx].step;
	pool_id = dp_pools[idx].id;
	dp_map_base = pool_base;
	dp_map_size = pool_size;
}

/* Last result, read back through sysfs. */
static char result[4096];
static size_t result_len;
static DEFINE_MUTEX(result_lock);

__printf(1, 2) static void say(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	result_len += vscnprintf(result + result_len,
				 sizeof(result) - result_len, fmt, args);
	va_end(args);
}

static void say_reset(void)
{
	result_len = 0;
	result[0] = '\0';
}

/*
 * Write a pattern over a range and read it back.
 *
 * Only ever called for a range a grow has just returned 0 for. memremap() rather than a plain
 * pointer because the pool is no-map: the kernel has no linear mapping for it, deliberately, so
 * that nothing wanders in by accident.
 */
static int verify_range(u64 offset, u64 len)
{
	void *va;
	u64 i;
	int ret = 0;

	va = memremap(pool_base + offset, len, MEMREMAP_WB);
	if (!va) {
		say("  memremap(%#llx+%#llx) failed\n", pool_base + offset, len);
		return -ENOMEM;
	}
	/* One word per 4 KiB: enough to catch a range that is only partly backed, without
	 * spending seconds writing tens of megabytes. */
	for (i = 0; i < len; i += 4096)
		*(u64 *)(va + i) = 0xD501D0000000ULL | (offset + i);
	for (i = 0; i < len; i += 4096) {
		u64 want = 0xD501D0000000ULL | (offset + i);
		u64 got = *(u64 *)(va + i);

		if (got != want) {
			say("  MISMATCH at +%#llx: wrote %#llx read %#llx%s\n",
			    offset + i, want, got,
			    got == 0 ? "  (zero: this range is NOT backed)" : "");
			ret = -EIO;
			break;
		}
	}
	memunmap(va);
	if (!ret)
		say("  verified %#llx+%#llx\n", offset, len);
	return ret;
}

/* Every way a request must be refused. Touches no memory, so it is safe to run at any time. */
static void test_rejections(void)
{
	u64 growable = pool_size - pool_prealloc;
	int rc;
	struct {
		const char *what;
		u64 off, len;
		int want;
	} cases[] = {
		{ "below the pre-shared floor", 0, pool_step, -EINVAL },
		{ "past the window", pool_size, pool_step, -EINVAL },
		{ "length past the window", pool_size - pool_step, pool_step * 2, -EINVAL },
		{ "misaligned offset", pool_prealloc + 4096, pool_step, -EINVAL },
		{ "misaligned length", pool_prealloc, pool_step + 4096, -EINVAL },
		{ "zero length", pool_prealloc, 0, -EINVAL },
		{ "releasing what was never granted", pool_prealloc, pool_step, -ENOENT },
	};
	int i;

	say("rejections:\n");
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		if (i == ARRAY_SIZE(cases) - 1)
			rc = gunyah_pool_shrink(pool_id, cases[i].off, cases[i].len);
		else
			rc = gunyah_pool_grow(pool_id, cases[i].off, cases[i].len);
		say("  %-34s -> %d %s\n", cases[i].what, rc,
		    rc == cases[i].want ? "ok" : "UNEXPECTED");
	}

	/* Overlap needs a live grant to overlap with. */
	if (growable >= pool_step * 2) {
		rc = gunyah_pool_grow(pool_id, pool_prealloc, pool_step);
		if (rc == 0) {
			rc = gunyah_pool_grow(pool_id, pool_prealloc, pool_step);
			say("  %-34s -> %d %s\n", "granting the same range twice", rc,
			    rc == -EEXIST ? "ok" : "UNEXPECTED");
			/* Starting BEFORE a live grant and running into it: the case a naive
			 * "is this offset taken" check misses. */
			rc = gunyah_pool_grow(pool_id, pool_prealloc, pool_step * 2);
			say("  %-34s -> %d %s\n", "a range enclosing a live grant", rc,
			    rc == -EEXIST ? "ok" : "UNEXPECTED");
			/* A grant is one memparcel and the RM reclaims it whole. */
			rc = gunyah_pool_shrink(pool_id, pool_prealloc, pool_step * 2);
			say("  %-34s -> %d %s\n", "releasing more than was taken", rc,
			    rc == -ERANGE || rc == -EINVAL ? "ok" : "UNEXPECTED");
			gunyah_pool_shrink(pool_id, pool_prealloc, pool_step);
		} else {
			say("  (skipped overlap cases: grow returned %d)\n", rc);
		}
	}
}

/*
 * A grant with something built over it must refuse to be released.
 *
 * The reference a real dma-buf import takes is only taken for pools with a non-zero step, and the
 * pool the GPU uses is fully pre-shared -- so without a way to take one by hand, this path could
 * not be reached on device at all. gunyah_pool_test_ref stands in for the import.
 */
static void test_busy(void)
{
	u64 off = pool_prealloc, len = pool_step;
	int rc;

	say("busy:\n");
	if (pool_step == 0 || pool_prealloc >= pool_size) {
		say("  pool is not growable; nothing to do\n");
		return;
	}

	rc = gunyah_pool_grow(pool_id, off, len);
	say("  grow                          -> %d\n", rc);
	if (rc)
		return;

	rc = gunyah_pool_test_ref(pool_id, off, len, true);
	say("  take a reference              -> %d %s\n", rc, rc == 0 ? "ok" : "UNEXPECTED");

	rc = gunyah_pool_shrink(pool_id, off, len);
	say("  shrink while referenced       -> %d %s\n", rc,
	    rc == -EBUSY ? "ok (refused)" : "UNEXPECTED -- it should have been refused");

	rc = gunyah_pool_test_ref(pool_id, off, len, false);
	say("  drop the reference            -> %d\n", rc);

	rc = gunyah_pool_shrink(pool_id, off, len);
	say("  shrink after dropping it      -> %d %s\n", rc, rc == 0 ? "ok" : "UNEXPECTED");

	/* A reference is refused outright over memory that was never granted -- the same check
	 * that stops a dma-buf being built over a hole in the sparse pool memfd. */
	rc = gunyah_pool_test_ref(pool_id, off, len, true);
	say("  reference an ungranted range  -> %d %s\n", rc,
	    rc == -EFAULT ? "ok (refused)" : "UNEXPECTED -- it should have been refused");
}

static void test_selftest(void)
{
	u64 off = pool_prealloc;
	u64 len = pool_step;
	u64 live = 0;
	int rc;

	say("selftest:\n");
	if (pool_step == 0) {
		say("  pool is not growable (step 0); nothing to do\n");
		return;
	}
	if (pool_prealloc >= pool_size) {
		say("  pool is fully pre-shared; nothing to grow\n");
		return;
	}

	rc = gunyah_pool_grow(pool_id, off, len);
	say("  grow %#llx+%#llx -> %d\n", off, len, rc);
	if (rc)
		return;
	if (verify_range(off, len))
		return;

	/* A second, larger grant: one memparcel however many steps it spans. */
	if (pool_size - pool_prealloc >= len + pool_step * 2) {
		u64 off2 = off + len, len2 = pool_step * 2;

		rc = gunyah_pool_grow(pool_id, off2, len2);
		say("  grow %#llx+%#llx -> %d  (2 steps, still ONE memparcel)\n",
		    off2, len2, rc);
		if (rc == 0) {
			verify_range(off2, len2);
			/* The first grant must still be intact: a second grow must not have
			 * disturbed it. */
			verify_range(off, len);
			rc = gunyah_pool_shrink(pool_id, off2, len2);
			say("  shrink %#llx+%#llx -> %d\n", off2, len2, rc);
		}
	}

	rc = gunyah_pool_query(pool_id, &live);
	say("  query -> %d, host says %llu live grant(s)\n", rc, live);

	rc = gunyah_pool_shrink(pool_id, off, len);
	say("  shrink %#llx+%#llx -> %d\n", off, len, rc);

	rc = gunyah_pool_query(pool_id, &live);
	say("  query -> %d, host says %llu live grant(s)\n", rc, live);
	say("  NOW CHECK THE HOST: served should have fallen back and pool_avail risen\n");
}

/*
 * Can the guest EXECUTE out of a runtime-granted range?
 *
 * This is the one question the pseudo-unprotected VM design turns on. A pool grant is an RM
 * memparcel whose ACL says what the guest may do with it, and the host asks for X only when
 * crosvm runs with GH_SHARE_EXEC=1. Nothing else in the stack cares: every pool today is data.
 *
 * Two ways in, because they answer subtly different questions and fail very differently:
 *
 *   EL0 (`/dev/dynpool` + mmap, driven by a userspace helper): a fault kills the helper and
 *        leaves the VM up. Run this one first.
 *   EL1 (`exec <off_mb>`, below): what the boot shim will actually do. A stage-2 XN here is an
 *        unhandled kernel fault -- the VM dies, which is a real answer but an expensive one.
 *
 * `mov w0, #42; ret`, called and compared against 42. The write and read-back of the same page
 * are the internal control: they prove the grant landed, so a fault on the CALL is about
 * execute permission and nothing else.
 */
static const u32 dp_exec_code[] = {
	0xd2800540,	/* mov x0, #42 */
	0xd65f03c0,	/* ret         */
};

/*
 * Accept a memparcel the host shared at an address no pool covers.
 *
 * GH_SHARE_PROBE=<gpa>:<kib> makes crosvm share a scratch range at a GPA of its caller's
 * choosing right after GH_VM_START and print the handle; with GH_SHARE_EXEC=1 that parcel's ACL
 * carries X. Accepting it here puts memory somewhere the guest was never told about -- no
 * reserved-memory node, no shm vdevice, no crosvm region -- which is exactly the shape of the
 * pseudo-unprotected window, and the one shape the sm8650-era RM has never been asked about.
 *
 * The host fills each page with its own GPA repeated, so reading that back proves the mapping
 * reaches the host's pages rather than merely being present.
 */
static void test_accept(u64 gpa, u64 size, u32 handle)
{
	void *va;
	u64 got;
	int rc;

	say("accept handle=%#x at %#llx+%#llx\n", handle, gpa, size);
	rc = gunyah_guest_mem_accept(handle, gpa, size);
	say("  MEM_ACCEPT -> %d%s\n", rc, rc ? "  (rejected)" : "");
	if (rc)
		return;
	/* Repoint /dev/dynpool here so the EL0 probe covers this range too. */
	dp_map_base = gpa;
	dp_map_size = size;
	say("  /dev/dynpool now maps %#llx+%#llx\n", gpa, size);

	va = memremap(gpa, PAGE_SIZE, MEMREMAP_WB);
	if (!va) {
		say("  memremap failed; cannot check the contents\n");
		return;
	}
	got = *(u64 *)va;
	say("  first word %#llx (host wrote the GPA repeated: %s)\n", got,
	    got == gpa ? "MATCH -- these are the host's pages" :
			 "MISMATCH -- not the host's pages");
	*(u64 *)va = ~got;
	say("  wrote back %#llx, read %#llx %s\n", ~got, *(u64 *)va,
	    *(u64 *)va == ~got ? "ok" : "MISMATCH");
	memunmap(va);
}

/* Accept, then execute, at an address outside every pool: the whole probe in one command so a
 * successful accept is never left dangling by a typo in the next one. */
static void test_accept_exec(u64 gpa, u64 size, u32 handle)
{
	int (*fn)(void);
	void __iomem *va;
	int rc;

	test_accept(gpa, size, handle);
	va = __ioremap_prot(gpa, PAGE_SIZE, PAGE_KERNEL_EXEC);
	if (!va) {
		say("  __ioremap_prot(EXEC) failed\n");
		return;
	}
	memcpy_toio(va, dp_exec_code, sizeof(dp_exec_code));
	if (memcmp((void __force *)va, dp_exec_code, sizeof(dp_exec_code))) {
		say("  code did not read back; NOT calling it\n");
		iounmap(va);
		return;
	}
	flush_icache_range((unsigned long)va,
			   (unsigned long)va + sizeof(dp_exec_code));
	say("  EL1: calling %#llx (a stage-2 XN takes the VM down here)\n", gpa);
	fn = (int (*)(void))(void __force *)va;
	rc = fn();
	say("  EL1: returned %d (expect 42) -> the guest CAN execute from this parcel\n", rc);
	iounmap(va);
}

static void test_exec_el1(u64 off)
{
	int (*fn)(void);
	void __iomem *va;
	u64 pa = dp_map_base + off;
	int got;

	/* dp_map_base is the active pool, or whatever `accept` last took: the EL1 probe follows
	 * the same window as the EL0 one, so the two are always talking about the same memory. */
	if (off >= dp_map_size) {
		say("offset %#llx is outside the current window (%#llx+%#llx)\n",
		    off, dp_map_base, dp_map_size);
		return;
	}
	/* PAGE_KERNEL_EXEC: Normal WB and, unlike memremap()'s PAGE_KERNEL, not PXN. ioremap
	 * rather than memremap because the pool is no-map -- there is no linear mapping to
	 * borrow, deliberately. */
	/* __ioremap_prot, not ioremap_prot: on arm64 the latter demands PTE_USER and then keeps
	 * only the memory-type bits of whatever it was handed, so everything it maps comes out as
	 * PAGE_KERNEL -- PXN, never executable (asm/io.h). The underlying __ioremap_prot takes the
	 * prot verbatim, which is the only way to ask for an EL1-executable mapping of a no-map
	 * range. PAGE_KERNEL_EXEC is Normal WB without PXN. */
	va = __ioremap_prot(pa, PAGE_SIZE, PAGE_KERNEL_EXEC);
	if (!va) {
		say("__ioremap_prot(%#llx, PAGE_KERNEL_EXEC) failed\n", pa);
		return;
	}
	memcpy_toio(va, dp_exec_code, sizeof(dp_exec_code));
	if (memcmp((void __force *)va, dp_exec_code, sizeof(dp_exec_code))) {
		say("code did not read back at %#llx -- the range is not backed; NOT calling it\n",
		    pa);
		iounmap(va);
		return;
	}
	flush_icache_range((unsigned long)va,
			   (unsigned long)va + sizeof(dp_exec_code));
	say("EL1: calling into %#llx (a stage-2 XN takes the VM down here)\n", pa);
	fn = (int (*)(void))(void __force *)va;
	got = fn();
	say("EL1: returned %d (expect 42) -> guest CAN execute from a granted range\n", got);
	iounmap(va);
}

/*
 * EL0 half: hand the granted range to userspace as an executable mapping. PAGE_SHARED_EXEC is
 * Normal WB with UXN clear, so the only thing left that can refuse the instruction fetch is
 * stage 2 -- which is what we are measuring.
 */
static int dp_mmap(struct file *f, struct vm_area_struct *vma)
{
	u64 len = vma->vm_end - vma->vm_start;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;

	if (!len || !dp_map_size || off >= dp_map_size || len > dp_map_size - off)
		return -EINVAL;
	vma->vm_page_prot = PAGE_SHARED_EXEC;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	return remap_pfn_range(vma, vma->vm_start,
			       (dp_map_base + off) >> PAGE_SHIFT, len,
			       vma->vm_page_prot);
}

static const struct file_operations dp_fops = {
	.owner = THIS_MODULE,
	.mmap  = dp_mmap,
	.llseek = noop_llseek,
};

static struct miscdevice dp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "dynpool",
	.fops  = &dp_fops,
	.mode  = 0600,
};

static ssize_t cmd_store(struct kobject *k, struct kobj_attribute *a,
			 const char *buf, size_t count)
{
	char verb[16];
	u64 a_mb = 0, b_mb = 0;
	int n, rc;

	mutex_lock(&result_lock);
	say_reset();

	n = sscanf(buf, "%15s %llu %llu", verb, &a_mb, &b_mb);
	if (n < 1) {
		say("could not parse a command\n");
		goto out;
	}

	/* `accept` and `acceptexec` take a hex GPA, a size in MB and the host's handle, so they
	 * need their own parse rather than the MB-and-MB one every other command shares. */
	/* `window` repoints the probes at a range this boot has ALREADY accepted -- the RM refuses
	 * a second accept of the same parcel, so after a module reload there is no way back to it
	 * except to say where it is. It maps nothing and accepts nothing. */
	if (!strcmp(verb, "window")) {
		u64 gpa = 0, size_mb = 0;

		if (sscanf(buf, "%15s %llx %llu", verb, &gpa, &size_mb) != 3) {
			say("usage: window <gpa_hex> <size_mb>\n");
			goto out;
		}
		dp_map_base = gpa;
		dp_map_size = size_mb * MB;
		say("window is now %#llx+%#llx (nothing was accepted)\n",
		    dp_map_base, dp_map_size);
		goto out;
	}

	if (!strcmp(verb, "accept") || !strcmp(verb, "acceptexec")) {
		u64 gpa = 0, size_mb = 0;
		u32 handle = 0;

		if (sscanf(buf, "%15s %llx %llu %x", verb, &gpa, &size_mb, &handle) != 4) {
			say("usage: %s <gpa_hex> <size_mb> <handle_hex>\n", verb);
			goto out;
		}
		if (!strcmp(verb, "accept"))
			test_accept(gpa, size_mb * MB, handle);
		else
			test_accept_exec(gpa, size_mb * MB, handle);
		goto out;
	}

	if (!strcmp(verb, "select") && n >= 2) {
		int i, hit = -1;
		for (i = 0; i < dp_npools; i++)
			if (dp_pools[i].id == (u32)a_mb)
				hit = i;
		if (hit < 0) {
			say("no pool with id %llu (have %d pools)\n", a_mb, dp_npools);
		} else {
			dp_activate(hit);
			say("selected pool_id %u (base %#llx, %llu MB, pre %llu MB, step %llu MB)\n",
			    pool_id, pool_base, pool_size / MB, pool_prealloc / MB, pool_step / MB);
		}
	} else if (!strcmp(verb, "selftest")) {
		test_selftest();
	} else if (!strcmp(verb, "reject")) {
		test_rejections();
	} else if (!strcmp(verb, "busy")) {
		test_busy();
	} else if (!strcmp(verb, "grow") && n == 3) {
		rc = gunyah_pool_grow(pool_id, a_mb * MB, b_mb * MB);
		say("grow %llu MB at +%llu MB -> %d\n", b_mb, a_mb, rc);
	} else if (!strcmp(verb, "shrink") && n == 3) {
		rc = gunyah_pool_shrink(pool_id, a_mb * MB, b_mb * MB);
		say("shrink %llu MB at +%llu MB -> %d\n", b_mb, a_mb, rc);
	} else if (!strcmp(verb, "verify") && n == 3) {
		verify_range(a_mb * MB, b_mb * MB);
	} else if (!strcmp(verb, "exec") && n >= 2) {
		test_exec_el1(a_mb * MB);
	} else {
		say("usage: selftest | reject | busy | grow <off_mb> <len_mb> | "
		    "shrink <off_mb> <len_mb> | verify <off_mb> <len_mb> | "
		    "exec <off_mb> | accept <gpa_hex> <size_mb> <handle_hex> | "
		    "acceptexec <gpa_hex> <size_mb> <handle_hex>\n");
	}
out:
	mutex_unlock(&result_lock);
	return count;
}

static ssize_t result_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	ssize_t n;

	mutex_lock(&result_lock);
	n = scnprintf(buf, PAGE_SIZE, "%s", result);
	mutex_unlock(&result_lock);
	return n;
}

static ssize_t info_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
			 "pool_id     %u\n"
			 "base        %#llx\n"
			 "size        %llu MB\n"
			 "pre_alloc   %llu MB   (backed before boot; never released)\n"
			 "step        %llu MB   (0 = not growable)\n"
			 "growable    %llu MB\n",
			 pool_id, pool_base, pool_size / MB, pool_prealloc / MB,
			 pool_step / MB, (pool_size - pool_prealloc) / MB);
}

static struct kobj_attribute cmd_attr = __ATTR(cmd, 0200, NULL, cmd_store);
static struct kobj_attribute result_attr = __ATTR(result, 0444, result_show, NULL);
static struct kobj_attribute info_attr = __ATTR(info, 0444, info_show, NULL);

static struct attribute *dp_attrs[] = {
	&cmd_attr.attr,
	&result_attr.attr,
	&info_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dp);

static int __init dp_init(void)
{
	struct device_node *np;
	struct resource res;
	int ret;

	/* Collect EVERY dynamic-pool node so both a two-pool layout and a single pool work. */
	np = NULL;
	for_each_compatible_node(np, NULL, "droidvm,dynamic-pool") {
		struct dp_pool *dp;

		if (dp_npools >= DP_MAX_POOLS)
			break;
		if (of_address_to_resource(np, 0, &res))
			continue;
		dp = &dp_pools[dp_npools];
		dp->base = res.start;
		/* `reg` is the pre-shared floor, not the window: android14-6.1's resource manager
		 * refuses a VM whose reserved-memory node describes a range no memparcel matches,
		 * and before boot only the floor is one. The window's size comes alongside; a pool
		 * that is fully pre-shared omits it, because there the floor is the window. */
		if (of_property_read_u64(np, "droidvm,pool-size", &dp->size))
			dp->size = resource_size(&res);
		/* Absent means "fully pre-shared": a pool that does not say how much to hold back
		 * is an ordinary non-growable one. */
		if (of_property_read_u64(np, "droidvm,pre-alloc-size", &dp->prealloc))
			dp->prealloc = resource_size(&res);
		if (of_property_read_u64(np, "droidvm,step-size", &dp->step))
			dp->step = 0;
		if (of_property_read_u32(np, "droidvm,pool-id", &dp->id))
			dp->id = 0;
		dp_npools++;
	}
	if (dp_npools == 0 && param_size) {
		struct dp_pool *dp = &dp_pools[dp_npools++];

		dp->base = param_base;
		dp->size = param_size;
		dp->prealloc = param_prealloc;
		dp->step = param_step ? param_step : param_size;
		dp->id = param_id;
		pr_info("dynpool_test: no DT node; using module parameters\n");
	}
	if (dp_npools == 0) {
		pr_info("dynpool_test: no droidvm,dynamic-pool node and no base=/size=; nothing to test\n");
		return -ENODEV;
	}
	dp_activate(0);

	if (!gunyah_guest_available())
		pr_warn("dynpool_test: RM not available; grow/shrink will fail\n");

	dp_kobj = kobject_create_and_add("dynpool_test", kernel_kobj);
	if (!dp_kobj)
		return -ENOMEM;
	ret = sysfs_create_groups(dp_kobj, dp_groups);
	if (ret) {
		kobject_put(dp_kobj);
		return ret;
	}
	/* Soft-fail: the sysfs commands are the module's job, /dev/dynpool only exists for the
	 * userspace execute probe. Refusing to load without it would take the rest away too. */
	ret = misc_register(&dp_misc);
	if (ret)
		pr_warn("dynpool_test: /dev/dynpool unavailable (%d); EL0 exec probe cannot run\n",
			ret);
	else
		dp_misc_ok = true;

	pr_info("dynpool_test: %d pool(s); active pool %u at %#llx, %llu MB (%llu MB pre-shared, step %llu MB)\n",
		dp_npools, pool_id, pool_base, pool_size / MB, pool_prealloc / MB, pool_step / MB);
	return 0;
}

static void __exit dp_exit(void)
{
	/* Deliberately does NOT release outstanding grants. A test that leaves memory granted
	 * should show up as such on the host -- unloading this module is not a licence to hide
	 * it, and the VM's teardown reclaims everything anyway. */
	if (dp_misc_ok)
		misc_deregister(&dp_misc);
	sysfs_remove_groups(dp_kobj, dp_groups);
	kobject_put(dp_kobj);
}

module_init(dp_init);
module_exit(dp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DroidVM: exercise a growable Gunyah memory pool");
MODULE_AUTHOR("DroidVM");
