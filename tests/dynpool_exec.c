// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright DroidVM contributors
// Additional permissions apply; see ADDITIONAL-PERMISSIONS in the repository root.

/*
 * Can this guest EXECUTE out of a runtime-granted Gunyah memparcel?
 *
 * The pseudo-unprotected VM design puts the guest's whole system RAM in a range the host
 * SHARE's at runtime and the guest MEM_ACCEPTs -- so the boot shim, the kernel, and everything
 * after it fetch instructions from a stage-2 mapping created by MEM_ACCEPT rather than by the
 * boot-time LEND. The host asks for X in the parcel's ACL only when crosvm runs with
 * GH_SHARE_EXEC=1; whether that survives the RM and the platform's SCM assign is the question.
 *
 * This is the EL0 half of the probe: dynpool_test's /dev/dynpool hands the range over as
 * PAGE_SHARED_EXEC (Normal WB, not UXN), so a refusal can only come from stage 2 -- and it
 * arrives as a signal in this process rather than as an unhandled fault that takes the VM down,
 * which is why this runs before the module's own `exec` command.
 *
 *   dynpool_exec <offset_mb>      (offset within the pool; must already be granted)
 *
 * Build in the guest:  cc -O2 -o dynpool_exec dynpool_exec.c
 */

#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf jump;
static volatile int caught;

static void on_fault(int sig)
{
	caught = sig;
	siglongjmp(jump, 1);
}

int main(int argc, char **argv)
{
	/* mov x0, #42 ; ret -- the same two instructions the in-kernel probe uses. */
	static const uint32_t code[] = { 0xd2800540, 0xd65f03c0 };
	unsigned long off_mb = argc > 1 ? strtoul(argv[1], NULL, 0) : 0;
	off_t off = (off_t)off_mb << 20;
	size_t len = 4096;
	int fd, (*fn)(void), got;
	void *p;

	fd = open("/dev/dynpool", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/dynpool");
		return 2;
	}

	p = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, fd, off);
	if (p == MAP_FAILED) {
		/* A noexec mount refuses PROT_EXEC at mmap time on a file mapping; the driver has
		 * already fixed the page protection, so asking for it afterwards gets there too. */
		perror("mmap PROT_EXEC (retrying without, then mprotect)");
		p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
		if (p == MAP_FAILED) {
			perror("mmap");
			return 2;
		}
		if (mprotect(p, len, PROT_READ | PROT_WRITE | PROT_EXEC)) {
			perror("mprotect");
			return 2;
		}
	}

	/* Control: the grant must already be usable as data. A fault or a bad read-back here is
	 * about the grant, not about execute permission, and the call below would be meaningless. */
	signal(SIGSEGV, on_fault);
	signal(SIGBUS, on_fault);
	signal(SIGILL, on_fault);
	if (sigsetjmp(jump, 1)) {
		printf("FAIL: signal %d while WRITING the range -- the grant is not usable at all\n",
		       caught);
		return 3;
	}
	memcpy(p, code, sizeof(code));
	if (memcmp(p, code, sizeof(code))) {
		printf("FAIL: the code did not read back -- range at +%luMB is not backed\n", off_mb);
		return 3;
	}
	__builtin___clear_cache((char *)p, (char *)p + sizeof(code));

	if (sigsetjmp(jump, 1)) {
		printf("NO-EXEC: signal %d on the CALL, while data access worked -- "
		       "stage 2 refuses instruction fetch from this parcel\n", caught);
		return 1;
	}
	fn = (int (*)(void))p;
	got = fn();
	if (got != 42) {
		printf("FAIL: called it and got %d, expected 42\n", got);
		return 3;
	}
	printf("EXEC OK: ran code at pool+%luMB from EL0 and it returned 42\n", off_mb);
	return 0;
}
