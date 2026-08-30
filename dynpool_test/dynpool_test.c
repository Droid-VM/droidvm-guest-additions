// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright DroidVM contributors
// Additional permissions apply; see ADDITIONAL-PERMISSIONS in the repository root.

/*
 * Accept a Gunyah memparcel at an address the guest was never told about, and find out
 * whether it can be executed.
 *
 * A pseudo-unprotected VM puts the guest's whole RAM in a range the host SHAREs at runtime and
 * the guest accepts -- so the kernel, and the shim that runs before it, fetch instructions from
 * a stage-2 mapping made by MEM_ACCEPT rather than by the boot-time LEND. Two questions have to
 * be answered on real hardware before any of that can be designed around, and neither can be
 * asked through a driver that only ever accepts what a reserved-memory node describes.
 *
 *   echo "accept <gpa_hex> <size_mb> <handle_hex>"     > /sys/kernel/dynpool_test/cmd
 *   echo "acceptexec <gpa_hex> <size_mb> <handle_hex>" > /sys/kernel/dynpool_test/cmd
 *   echo "window <gpa_hex> <size_mb>"                  > /sys/kernel/dynpool_test/cmd
 *   echo "exec <off_mb>"                               > /sys/kernel/dynpool_test/cmd
 *   cat /sys/kernel/dynpool_test/result
 */

#include <linux/module.h>
#include <linux/kernel.h>
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

static struct kobject *dp_kobj;
static bool dp_misc_ok;
/* What /dev/dynpool maps: the range `accept` just took, which is outside every pool and is
 * the case the pseudo-unprotected window cares about. */
static u64 dp_map_base, dp_map_size;

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
	int n;

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

	if (!strcmp(verb, "exec") && n >= 2) {
		test_exec_el1(a_mb * MB);
	} else {
		say("usage: exec <off_mb> | window <gpa_hex> <size_mb> | "
		    "accept <gpa_hex> <size_mb> <handle_hex> | "
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

static struct kobj_attribute cmd_attr = __ATTR(cmd, 0200, NULL, cmd_store);
static struct kobj_attribute result_attr = __ATTR(result, 0444, result_show, NULL);

static struct attribute *dp_attrs[] = {
	&cmd_attr.attr,
	&result_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dp);

static int __init dp_init(void)
{
	int ret;

	if (!gunyah_guest_available())
		pr_warn("dynpool_test: RM not available; accept will fail\n");

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

	pr_info("dynpool_test: ready; nothing is mapped until `window` or `accept` says where\n");
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
MODULE_DESCRIPTION("DroidVM: accept a Gunyah memparcel anywhere and probe execute permission");
MODULE_AUTHOR("DroidVM");
