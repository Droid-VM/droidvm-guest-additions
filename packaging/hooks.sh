#!/bin/sh
# Install/remove logic shared by the .deb maintainer scripts and the .rpm scriptlets.
#
# One implementation on purpose. dpkg and rpm hand their scriptlets different arguments and run
# them at different points, but what has to HAPPEN is identical, and the details that matter here
# were each learned from a specific failure -- duplicating them into two packaging formats is how
# one copy quietly loses a fix. The format-specific files are thin wrappers that decide *whether*
# to call these functions; everything about *what* they do lives here.
#
# @PKG@ and @VER@ are substituted at package build time.
set -e

PKG=@PKG@
VER=@VER@

ga_msg()  { echo "$PKG: $*"; }
ga_warn() { echo "$PKG: warning: $*" >&2; }

# The initramfs is not optional and not cosmetic. virtio-gpu is loaded by early KMS from the
# initramfs, so a guest whose initramfs still holds the in-tree driver boots the in-tree driver no
# matter what is on the root filesystem -- and `modinfo -F filename virtio_gpu` points at ours the
# whole time, which is what makes it look like the module did not take effect rather than like the
# initramfs is stale.
#
# This is the package's job, not dkms's: dkms 3.2.2 deprecated REMAKE_INITRD and now IGNORES it,
# with only a "Deprecated feature" line to say so -- an install that logs it leaves the initramfs
# untouched and looks like it succeeded.
#
# The kernel-UPGRADE path is not ours: /etc/kernel/postinst.d/dkms runs dkms_autoinstaller for the
# new kernel before that kernel's initramfs is generated. What is ours is right now -- install and
# removal against the running kernel.
#
# -k "$1" rather than -u: `update-initramfs -u` rebuilds for the NEWEST installed kernel, which on
# a guest that has pulled a kernel update is not the one running. That reports success while
# rebuilding an initrd nothing boots.
ga_remake_initrd() {
	kver=${1:-$(uname -r)}
	if command -v update-initramfs >/dev/null 2>&1; then
		ga_msg "updating initramfs for $kver"
		update-initramfs -u -k "$kver" || ga_warn "update-initramfs failed"
	elif command -v dracut >/dev/null 2>&1; then
		ga_msg "regenerating initramfs for $kver"
		dracut -f --kver "$kver" || ga_warn "dracut failed"
	else
		ga_warn "no update-initramfs and no dracut: rebuild the initramfs yourself," \
		        "or early KMS will keep loading the previous virtio-gpu"
	fi
}

# A blacklist entry acts on the module NAME, so one left over from a pre-DKMS manual-load setup
# blocks our module too -- the package installs cleanly and the driver never autoloads.
ga_check_blacklist() {
	if grep -rlsE '^[[:space:]]*blacklist[[:space:]]+virtio[-_]gpu' /etc/modprobe.d/ >/dev/null 2>&1; then
		ga_warn "virtio-gpu is blacklisted in /etc/modprobe.d/; this package needs no blacklist" \
		        "and will not autoload while one is present:"
		grep -rlsE '^[[:space:]]*blacklist[[:space:]]+virtio[-_]gpu' /etc/modprobe.d/ >&2 || true
	fi
}

ga_install() {
	# Idempotent: a reinstall or a failed previous attempt must not wedge the tree.
	if dkms status -m "$PKG" -v "$VER" 2>/dev/null | grep -q .; then
		dkms remove -m "$PKG" -v "$VER" --all >/dev/null 2>&1 || true
	fi

	# Also drop OTHER versions of this module that no package owns. The pre-packaging installer
	# registered a bare "1.0" straight into /usr/src, and a guest that ran it still has that
	# registration -- dpkg and rpm know nothing about it, so nothing else will ever clean it up.
	# Left alone, both versions AUTOINSTALL on the next kernel upgrade and race for the same
	# updates/dkms path, and which one wins is decided by dkms's iteration order.
	# dkms 3.x prints "name/version, kernel, arch: state"; 2.x prints "name, version, kernel, ...".
	# Stripping the name with either separator and then everything from the next , or : leaves the
	# version under both.
	for other in $(dkms status -m "$PKG" 2>/dev/null \
	               | sed -e "s|^$PKG[/,] *||" -e 's|[,:].*||' | sort -u); do
		[ -n "$other" ] && [ "$other" != "$VER" ] || continue
		ga_msg "removing unowned dkms registration $PKG/$other"
		dkms remove -m "$PKG" -v "$other" --all >/dev/null 2>&1 || true
		rm -rf "/usr/src/$PKG-$other"
	done

	dkms add -m "$PKG" -v "$VER" || {
		ga_warn "dkms add failed; the modules are NOT installed"
		return 0
	}

	# Never fail the package install on a build failure. An unbuildable module is a broken
	# feature; a failed transaction on `dnf install` or `apt install` is a broken system, and
	# leaves the user with no package to remove either. Warn loudly instead, and be specific:
	# dkms.conf bounds builds with BUILD_EXCLUSIVE_KERNEL, so "no modules" is a routine outcome
	# on an unsupported kernel and needs to say so rather than look like a compiler error.
	if ! dkms install -m "$PKG" -v "$VER" --force; then
		case "$(uname -r)" in
		7.*) ga_warn "dkms build/install failed on kernel $(uname -r) -- see /var/lib/dkms/$PKG/$VER/build/make.log" ;;
		*)   ga_warn "kernel $(uname -r) is outside the range dkms.conf supports" \
		             "(BUILD_EXCLUSIVE_KERNEL=^7\\.), so nothing was built. The package is" \
		             "installed and will build automatically once a 7.x kernel is present." ;;
		esac
		return 0
	fi

	# Only after a successful install: rebuilding the initramfs when nothing was built would
	# bake in whatever is there now and report progress for a no-op.
	ga_remake_initrd "$(uname -r)"

	ga_check_blacklist

	# Prove the displacement worked rather than asserting it. Our module has the same NAME as the
	# in-tree one and wins only because depmod ranks updates/dkms first; if that ever stops being
	# true the symptom is silent (stock driver, no accept, SIGBUS on the first host-visible blob).
	inst=$(modinfo -F filename virtio_gpu 2>/dev/null || true)
	case "$inst" in
	*/updates/dkms/*) ga_msg "virtio_gpu resolves to $inst" ;;
	*) ga_warn "virtio_gpu resolves to '${inst:-not found}', expected .../updates/dkms/" ;;
	esac

	ga_msg "installed. Reboot, then check: dmesg | grep -e gunyah_guest -e 'virtio-gpu'"
}

ga_remove() {
	if dkms status -m "$PKG" -v "$VER" 2>/dev/null | grep -q .; then
		ga_msg "removing dkms modules"
		dkms remove -m "$PKG" -v "$VER" --all || ga_warn "dkms remove failed"
		# The in-tree virtio-gpu takes over from here, so the initramfs has to stop carrying
		# ours -- otherwise early KMS keeps loading a module the system no longer has sources
		# for, and a later kernel upgrade boots with a driver nothing can rebuild.
		ga_remake_initrd "$(uname -r)"
	fi
}
