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
#include <linux/gunyah_guest.h>

#define MB (1UL << 20)

static u64 pool_base, pool_size, pool_prealloc, pool_step;
static u32 pool_id;
static struct kobject *dp_kobj;

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

	if (!strcmp(verb, "selftest")) {
		test_selftest();
	} else if (!strcmp(verb, "reject")) {
		test_rejections();
	} else if (!strcmp(verb, "grow") && n == 3) {
		rc = gunyah_pool_grow(pool_id, a_mb * MB, b_mb * MB);
		say("grow %llu MB at +%llu MB -> %d\n", b_mb, a_mb, rc);
	} else if (!strcmp(verb, "shrink") && n == 3) {
		rc = gunyah_pool_shrink(pool_id, a_mb * MB, b_mb * MB);
		say("shrink %llu MB at +%llu MB -> %d\n", b_mb, a_mb, rc);
	} else if (!strcmp(verb, "verify") && n == 3) {
		verify_range(a_mb * MB, b_mb * MB);
	} else {
		say("usage: selftest | reject | grow <off_mb> <len_mb> | "
		    "shrink <off_mb> <len_mb> | verify <off_mb> <len_mb>\n");
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

	np = of_find_compatible_node(NULL, NULL, "droidvm,dynamic-pool");
	if (!np) {
		pr_info("dynpool_test: no droidvm,dynamic-pool node; nothing to test\n");
		return -ENODEV;
	}
	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		of_node_put(np);
		return ret;
	}
	pool_base = res.start;
	pool_size = resource_size(&res);
	/* Absent means "fully pre-shared", matching the host's own default: a pool that does not
	 * say how much to hold back is an ordinary non-growable one. */
	if (of_property_read_u64(np, "droidvm,pre-alloc-size", &pool_prealloc))
		pool_prealloc = pool_size;
	if (of_property_read_u64(np, "droidvm,step-size", &pool_step))
		pool_step = 0;
	if (of_property_read_u32(np, "droidvm,pool-id", &pool_id))
		pool_id = 0;
	of_node_put(np);

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

	pr_info("dynpool_test: pool %u at %#llx, %llu MB (%llu MB pre-shared, step %llu MB)\n",
		pool_id, pool_base, pool_size / MB, pool_prealloc / MB, pool_step / MB);
	return 0;
}

static void __exit dp_exit(void)
{
	/* Deliberately does NOT release outstanding grants. A test that leaves memory granted
	 * should show up as such on the host -- unloading this module is not a licence to hide
	 * it, and the VM's teardown reclaims everything anyway. */
	sysfs_remove_groups(dp_kobj, dp_groups);
	kobject_put(dp_kobj);
}

module_init(dp_init);
module_exit(dp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DroidVM: exercise a growable Gunyah memory pool");
MODULE_AUTHOR("DroidVM");
