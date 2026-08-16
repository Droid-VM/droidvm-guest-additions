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
	# A kernel that is being installed in the SAME apt transaction (its headers configured before
	# us, its linux-modules/linux-image not yet) has no modules.dep and no vmlinuz yet: dracut
	# would fail with "modules.dep is missing", and the initrd is that kernel's own postinst
	# trigger's job anyway -- it runs later in the transaction and picks up the DKMS module we
	# just installed under updates/dkms. Say so instead of emitting a spurious warning.
	if [ ! -e "/lib/modules/$kver/modules.dep" ] || [ ! -e "/boot/vmlinuz-$kver" ]; then
		ga_msg "kernel $kver is still being installed; its own postinst will build the initramfs"
		return 0
	fi
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

	# Build for + refresh the initramfs of EVERY installed kernel that has headers, not just the
	# running one. `apt install` of this package (and the provisioner) pull linux-headers-generic,
	# which drags in a NEWER linux-image; that kernel's initrd was generated by its own postinst --
	# in the same transaction, before this module was registered -- so it baked the STOCK in-tree
	# virtio-gpu, and it is the kernel GRUB boots. Early KMS then loads the stock module (no
	# gfx_host / drm2kgsl_host / venus_host reserved-memory pool node), the DKMS copy on the rootfs
	# never binds the already-claimed device, and every pool-resident route (gfxstream host pool,
	# drm2kgsl arena, venus transport) reads gpu_pool_base=0 and SIGBUSes deep in the GPU stack --
	# with modinfo pointing at our file the whole time. Handling only "$(uname -r)" is exactly why
	# a guest that booted a freshly-pulled kernel came up on the stock driver; the assumption that
	# /etc/kernel/postinst.d/dkms covers the new kernel fails when this package is not registered
	# yet at the moment that kernel's postinst runs. Do every 7.x kernel with headers ourselves.
	#
	# Never fail the package install on a build failure: an unbuildable module is a broken feature,
	# but a failed `apt install`/`dnf install` transaction is a broken system with no package left
	# to remove. dkms.conf bounds builds with BUILD_EXCLUSIVE_KERNEL=^7\., so a non-7.x kernel
	# building nothing is routine, not an error.
	built_any=
	built_running=
	running=$(uname -r)
	for k in $(ls /lib/modules 2>/dev/null); do
		[ -e "/lib/modules/$k/build" ] || continue	# no headers for this kernel -> dkms cannot build it
		case "$k" in 7.*) ;; *) continue ;; esac	# BUILD_EXCLUSIVE_KERNEL=^7\.
		if dkms install -m "$PKG" -v "$VER" -k "$k" --force >/dev/null 2>&1; then
			built_any=1
			[ "$k" = "$running" ] && built_running=1
			# Only after a successful build: regenerating with nothing built would bake in the
			# stock driver and report progress for a no-op.
			ga_remake_initrd "$k"
		fi
	done
	if [ -z "$built_any" ]; then
		case "$running" in
		7.*) ga_warn "dkms build/install failed on kernel $running -- see /var/lib/dkms/$PKG/$VER/build/make.log" ;;
		*)   ga_warn "kernel $running is outside the range dkms.conf supports" \
		             "(BUILD_EXCLUSIVE_KERNEL=^7\\.), so nothing was built. The package is" \
		             "installed and will build automatically once a 7.x kernel is present." ;;
		esac
		return 0
	fi

	ga_check_blacklist

	# The loop builds for every kernel that HAS headers. linux-headers-generic -- this package's
	# dependency -- is a META-package that only pulls headers for the archive's NEWEST kernel, so on
	# a guest whose running kernel is behind the archive (a stale image, or one that pulled a newer
	# kernel and has not yet rebooted into it) the module is built for that newest kernel but NOT for
	# the one running now. Early KMS then keeps the stock virtio-gpu and every host-visible route
	# SIGBUSes, with modinfo pointing at our file the whole time. dkms cannot fetch the running
	# kernel's headers itself -- its build runs under the package manager's lock -- so, exactly as
	# nvidia-dkms and virtualbox-dkms do, say what to run rather than fail. Booting into the newer
	# kernel that DID build is the other fix, which is why the install still succeeds.
	if [ -z "$built_running" ]; then
		case "$running" in
		7.*)
			ga_warn "kernel headers for the running kernel ($running) are not installed, so its" \
			        "module was not built and the stock virtio-gpu is in use. Install them and rebuild:"
			ga_warn "    sudo apt-get install linux-headers-$running"
			ga_warn "    sudo dkms autoinstall && sudo update-initramfs -u -k $running && sudo reboot"
			;;
		esac
		return 0
	fi

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
