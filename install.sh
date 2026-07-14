#!/bin/bash
# DroidVM guest additions installer. Run INSIDE the guest, as root:
#
#   curl -L https://raw.githubusercontent.com/Droid-VM/droidvm-guest-additions/3d-accel-gfxstream/install.sh | sudo bash
#
# or from a checkout: sudo ./install.sh
#
# Installs the gunyah_guest + patched virtio-gpu modules as a DKMS package
# (auto-rebuilds on kernel upgrades) and refreshes the initramfs so early KMS
# picks up the patched driver. Optionally unpacks a guest mesa tarball
# (gfxstream ICD + zink) if DROIDVM_MESA_URL is set.
#
# Env overrides: DROIDVM_GA_REPO (owner/repo), DROIDVM_GA_REF (branch/tag),
# DROIDVM_MESA_URL (mesa-guest-aarch64.tar.gz URL, unpacked to / + ldconfig).
set -euo pipefail

REPO="${DROIDVM_GA_REPO:-Droid-VM/droidvm-guest-additions}"
REF="${DROIDVM_GA_REF:-3d-accel-gfxstream}"
PKG=droidvm-guest-additions

msg()  { echo "==> $*"; }
warn() { echo "warning: $*" >&2; }
die()  { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must run as root (curl -L ... | sudo bash)"
[ "$(uname -m)" = aarch64 ] || die "expected an aarch64 guest, got $(uname -m)"
command -v apt-get >/dev/null || die "only Debian/Ubuntu guests are supported (apt-get not found)"

case "$(uname -r)" in
7.*) ;;
*) warn "kernel $(uname -r) is not 7.x; dkms.conf bounds builds to 7.x" \
        "(BUILD_EXCLUSIVE_KERNEL) so the modules will NOT be built for this kernel" ;;
esac

# Purely informational: without the RM node gunyah_guest stays dormant (safe).
if ! grep -qrls gunyah-resource-manager /proc/device-tree 2>/dev/null; then
	warn "no gunyah-resource-manager DT node: not a Gunyah VM?" \
	     "Modules install fine but GuestAccept will be disabled at runtime."
fi

msg "installing build dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq dkms build-essential "linux-headers-$(uname -r)" \
	curl ca-certificates

# Source: the directory this script lives in if it's a checkout (has dkms.conf),
# otherwise fetch the repo tarball (the curl | bash path).
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
script_dir=$(dirname "$(readlink -f "${BASH_SOURCE[0]:-/dev/null}")")
if [ -f "$script_dir/dkms.conf" ]; then
	srcdir="$script_dir"
	msg "using local checkout at $srcdir"
else
	msg "fetching $REPO @ $REF"
	curl -fsSL "https://github.com/$REPO/archive/$REF.tar.gz" -o "$tmp/src.tgz"
	tar -xzf "$tmp/src.tgz" -C "$tmp"
	srcdir=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -n1)
	[ -f "$srcdir/dkms.conf" ] || die "downloaded tree has no dkms.conf"
fi

ver=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p' "$srcdir/dkms.conf")
[ -n "$ver" ] || die "cannot read PACKAGE_VERSION from dkms.conf"

# Idempotent re-run: drop any previously registered build/install first.
if dkms status 2>/dev/null | grep -q "^${PKG}[/,]"; then
	msg "removing previously installed $PKG"
	dkms remove "$PKG/$ver" --all 2>/dev/null || true
fi
rm -rf "/usr/src/$PKG-$ver"
mkdir -p "/usr/src/$PKG-$ver"
# .git never ships in the tarball; strip it for the checkout path too.
(cd "$srcdir" && tar -c --exclude .git .) | tar -x -C "/usr/src/$PKG-$ver"

msg "dkms install $PKG/$ver (gunyah_guest + patched virtio-gpu)"
dkms install "$PKG/$ver"

# Early KMS loads virtio-gpu from the initramfs; refresh it so the DKMS copy
# (updates/dkms outranks in-tree in depmod) is the one baked in.
msg "updating initramfs"
update-initramfs -u

# A leftover blacklist from the pre-DKMS manual-load setup would also block
# the DKMS module (blacklists act on the module name, not the path).
if grep -rlsE '^\s*blacklist\s+virtio[-_]gpu' /etc/modprobe.d/ >/dev/null 2>&1; then
	warn "virtio-gpu is blacklisted in /etc/modprobe.d/ — remove that entry," \
	     "the DKMS module needs no blacklist and won't autoload with it in place:"
	grep -rlsE '^\s*blacklist\s+virtio[-_]gpu' /etc/modprobe.d/ >&2
fi

if [ -n "${DROIDVM_MESA_URL:-}" ]; then
	msg "installing guest mesa from $DROIDVM_MESA_URL"
	curl -fL "$DROIDVM_MESA_URL" | tar -xz -C /
	ldconfig
fi

inst=$(modinfo -F filename virtio_gpu 2>/dev/null || true)
case "$inst" in
*/updates/dkms/*) msg "virtio-gpu now resolves to $inst" ;;
*) warn "virtio_gpu resolves to '${inst:-not found}', expected .../updates/dkms/" ;;
esac

msg "done — reboot the guest to load the patched driver, then check:"
echo "    dmesg | grep gunyah_guest   # expect: RM client ready (tx_capid=...)"
echo "    modinfo -F filename virtio_gpu"
