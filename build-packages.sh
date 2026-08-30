#!/bin/bash
# Package the guest additions (virtio-gpu + gunyah_guest, i.e. the virtio-gunyah-accept transport)
# as a DKMS source package, for both distro families:
#
#   droidvm-guest-additions_<ver>_arm64.deb          Debian / Ubuntu
#   droidvm-guest-additions-<ver>-1.aarch64.rpm      Fedora / RHEL
#
#   ./build-packages.sh [deb|rpm|both]      (default: both)
#
# Output goes to $OUTDIR (default: this directory).
#
# WHY A SOURCE PACKAGE IS THE EASY HALF
#
# Nothing is compiled here. Both packages ship the same C sources to /usr/src/<pkg>-<ver> and let
# dkms build them ON THE GUEST, against that guest's own kernel headers. So unlike the mesa
# packages -- which ship binaries and therefore have to be built once per distro against that
# distro's glibc -- one payload is genuinely correct on both families, and the only thing that
# differs between the two packages is the dependency names and the scriptlet syntax.
#
# The install/remove logic itself is NOT duplicated: both formats call packaging/hooks.sh.
#
# rpmbuild is not needed on the host; if it is missing, a fedora container is used instead. The
# rpm contains no compiled objects, so building it on x86_64 with --target aarch64 is exact, not
# an approximation -- no emulation involved.
set -euo pipefail
cd "$(dirname "$0")"

WHAT=${1:-both}
PKG=droidvm-guest-additions
OUTDIR=${OUTDIR:-$PWD}
FEDORA_IMG=${FEDORA_IMG:-fedora:42}

msg() { echo "==> $*"; }
die() { echo "error: $*" >&2; exit 1; }

[ -f dkms.conf ] || die "run this from the droidvm-guest-additions checkout"

# One version string for the deb Version, the rpm Version, the dkms PACKAGE_VERSION and therefore
# the /usr/src directory name. Keeping them equal is what makes `dkms status` name the exact tree
# a guest is running, and what lets two builds of "1.0" coexist in a log without ambiguity.
# rpm allows '+' in Version but not '-', which is why the separator is a dot.
#
# A commit count leads and the hash only identifies: a package manager compares versions, and a
# hash does not order. "+droidvm.cea49934" is LOWER than "+droidvm.f80a84b5" no matter which was
# built first, so upgrading to a newer build needed --allow-downgrades and `apt upgrade` could
# quietly keep the older one. Counting commits gives a number that only goes up, on a branch that
# is never rewritten.
#
# The "r" is what makes the change of scheme itself an upgrade rather than a downgrade: dpkg would
# otherwise compare the letters of an old "cea49934" against the empty run before "250" and rank
# the letters higher. A hash is hex and starts with 0-9 or a-f, so any letter past 'f' outranks
# every version already installed. (rpm ranks a digit segment above an alpha one, so an old rpm
# whose hash began with a digit would still need --oldpackage; the rpm path is not one we ship.)
#
# A modified tree gets a suffix as well, because otherwise an uncommitted change rebuilds to the
# same filename with different contents -- a package that installs a build you can no longer
# identify. It sorts after the clean build of that commit and before the next commit, which is
# exactly where it belongs.
base=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p' dkms.conf)
[ -n "$base" ] || die "cannot read PACKAGE_VERSION from dkms.conf"
count=$(git rev-list --count HEAD 2>/dev/null || echo 0)
sha=$(git rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
dirty=""
git diff --quiet HEAD -- 2>/dev/null || dirty="+dirty$(LC_ALL=C date -u '+%Y%m%d%H%M%S')"
VER="${base}+droidvm.r${count}.g${sha}${dirty}"
msg "version $VER"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
SRCDIR="$STAGE/root/usr/src/$PKG-$VER"

# ---------------------------------------------------------------------------
# Payload, identical for both formats.
# ---------------------------------------------------------------------------
mkdir -p "$SRCDIR"
# Ship the tree as-is minus VCS and build leftovers. "Ship the full source" is the deliberate
# policy for this package (see the repo README), so the exclude list stays minimal: anything
# dropped here is something a guest cannot rebuild from.
tar -c --exclude=.git --exclude='*.o' --exclude='*.ko' --exclude='*.mod*' \
    --exclude='.*.cmd' --exclude=Module.symvers --exclude=modules.order \
    --exclude='*.deb' --exclude='*.rpm' . | tar -x -C "$SRCDIR"

# The /usr/src directory name and PACKAGE_VERSION must agree or dkms cannot find its own source.
sed -i "s/^PACKAGE_VERSION=\".*\"$/PACKAGE_VERSION=\"$VER\"/" "$SRCDIR/dkms.conf"

install -d -m 0755 "$STAGE/root/usr/libexec/$PKG"
sed -e "s/@PKG@/$PKG/g" -e "s/@VER@/$VER/g" packaging/hooks.sh \
    > "$STAGE/root/usr/libexec/$PKG/hooks.sh"
chmod 0644 "$STAGE/root/usr/libexec/$PKG/hooks.sh"

# ---------------------------------------------------------------------------
# .deb
# ---------------------------------------------------------------------------
build_deb() {
    local root="$STAGE/deb"
    rm -rf "$root"; cp -a "$STAGE/root" "$root"
    install -d -m 0755 "$root/DEBIAN"

    cat > "$root/DEBIAN/control" <<EOF
Package: $PKG
Version: $VER
Section: kernel
Priority: optional
Architecture: arm64
Installed-Size: $(du -sk "$root" | cut -f1)
Maintainer: Droid-VM <noreply@github.com>
Depends: dkms, gcc, make, kmod, linux-headers-generic | linux-headers-arm64
Description: DroidVM guest kernel modules (DKMS)
 Builds and installs two modules on the guest:
 .
  * virtio-gpu   - the DroidVM virtio-gpu driver, which knows about the
                   host-owned and guest-owned memory pools the DT describes.
                   It replaces the in-tree driver of the same name by ranking
                   ahead of it in depmod's search order.
  * gunyah_guest - the Gunyah resource-manager client and the
                   virtio-gunyah-accept transport the host drives to accept
                   memparcels on the guest's behalf.
 .
 Sources are installed to /usr/src and built by dkms against the running
 kernel, and rebuilt automatically whenever the guest's kernel is upgraded.
EOF

    # linux-headers-generic is the META-package, and depending on it rather than on the versioned
    # headers for today's kernel is the whole point. The versioned package covers exactly one
    # kernel: when the guest later pulls a kernel update, headers for THAT kernel never arrive,
    # dkms cannot build, and the new kernel boots with no virtio-gpu and no gunyah_guest. This
    # guest reached exactly that state once; the workaround was pinning GRUB to the old kernel,
    # which then made `update-initramfs -u` silently rebuild an initrd nothing boots.
    # The alternative covers plain Debian arm64, where the meta-package has a different name.

    cat > "$root/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
[ "\$1" = configure ] || exit 0
. /usr/libexec/$PKG/hooks.sh
ga_install
EOF
    # prerm, not postrm: the sources under /usr/src must still exist when dkms removes the built
    # modules, and by postrm dpkg has already deleted them -- dkms then cannot read its own
    # dkms.conf and leaves the built modules installed with nothing owning them.
    #
    # `remove` only, NOT `upgrade`. Tearing down mid-upgrade removes what the new package's
    # postinst is about to rebuild, and each teardown regenerates the initramfs -- about a minute
    # on a guest -- so an upgrade paid for two rebuilds to reach the same place. The old dkms
    # version is cleaned up by the new postinst instead (it drops every registered version that
    # is not its own). This is the same rule the rpm expresses as [ "$1" = 0 ] in %preun.
    cat > "$root/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
case "\$1" in remove|deconfigure) ;; *) exit 0 ;; esac
. /usr/libexec/$PKG/hooks.sh
ga_remove
EOF
    chmod 0755 "$root/DEBIAN/postinst" "$root/DEBIAN/prerm"

    local deb="${PKG}_${VER}_arm64.deb"
    dpkg-deb --root-owner-group --build "$root" "$OUTDIR/$deb" >/dev/null
    msg "wrote $deb"
}

# ---------------------------------------------------------------------------
# .rpm
# ---------------------------------------------------------------------------
write_spec() {
    cat > "$1" <<EOF
%global pkgname $PKG
%global debug_package %{nil}
# No compiled content: nothing to strip, nothing to scan for shared-library deps, and the C
# sources must not be turned into requirements on a compiler by the file classifier.
%global __brp_strip %{nil}
%global __brp_strip_static_archive %{nil}
AutoReqProv: no

Name:           %{pkgname}
Version:        $VER
# No %{?dist}. One rpm serves the whole family, and the dist tag would name the Fedora release of
# the BUILD CONTAINER rather than the guest -- a package built in fedora:42 and installed on
# fedora:43 would advertise .fc42 and read as the wrong build.
Release:        1
Summary:        DroidVM guest kernel modules (DKMS)
License:        GPL-2.0-only
URL:            https://github.com/Droid-VM/droidvm-guest-additions
# Deliberately NO BuildArch and NO ExclusiveArch. The package is tagged aarch64 -- the payload is
# pure source, but these modules only exist for aarch64 Gunyah guests, so the tag is what stops an
# x86_64 host installing something that can never build. That tag comes from `rpmbuild --target
# aarch64` instead: both spec tags are validated against the BUILD host, which here is x86_64, and
# either one fails with "No compatible architectures found for build" -- an error that reads like
# a spec mistake rather than a cross-build one.

Requires:       dkms
Requires:       gcc
Requires:       make
Requires:       kmod
# The Fedora analogue of Debian's linux-headers-generic: kernel-devel-matched follows the
# installed kernel, so a kernel upgrade brings its headers along and dkms AUTOINSTALL can
# rebuild unprompted. Plain kernel-devel is the fallback for older rpm without rich deps
# and for rebuilds pinned to one kernel.
Requires:       (kernel-devel-matched or kernel-devel)

%description
Builds and installs two modules on the guest:

 * virtio-gpu   - the DroidVM virtio-gpu driver, which knows about the
                  host-owned and guest-owned memory pools the DT describes.
                  It replaces the in-tree driver of the same name by ranking
                  ahead of it in depmod's search order.
 * gunyah_guest - the Gunyah resource-manager client and the
                  virtio-gunyah-accept transport the host drives to accept
                  memparcels on the guest's behalf.

Sources are installed to /usr/src and built by dkms against the running
kernel, and rebuilt automatically whenever the guest's kernel is upgraded.

%prep
%build

%install
cp -a %{_sourcedir}/payload/. %{buildroot}/

%files
/usr/src/%{pkgname}-$VER
%dir /usr/libexec/%{pkgname}
/usr/libexec/%{pkgname}/hooks.sh

%post
. /usr/libexec/%{pkgname}/hooks.sh
# \$1 is 1 on install and 2 on upgrade; ga_install is idempotent, so both take the same path.
ga_install

%preun
# \$1 is 0 on a real removal and 1 on the remove half of an upgrade. Only tear the modules down
# for a real removal: doing it mid-upgrade removes what the new package's %post is about to
# rebuild, and on a kernel that fails to build leaves the guest with nothing.
if [ "\$1" = 0 ]; then
    . /usr/libexec/%{pkgname}/hooks.sh
    ga_remove
fi

%changelog
* $(LC_ALL=C date -u '+%a %b %d %Y') Droid-VM <noreply@github.com> - $VER-1
- Packaged from git $sha
EOF
}

build_rpm() {
    local rpmroot="$STAGE/rpmbuild"
    mkdir -p "$rpmroot"/{SPECS,SOURCES,BUILD,RPMS,SRPMS,BUILDROOT}
    cp -a "$STAGE/root" "$rpmroot/SOURCES/payload"
    write_spec "$rpmroot/SPECS/$PKG.spec"

    if command -v rpmbuild >/dev/null 2>&1; then
        msg "building rpm with the host rpmbuild"
        rpmbuild --define "_topdir $rpmroot" --target aarch64 -bb "$rpmroot/SPECS/$PKG.spec" >/dev/null
    else
        command -v docker >/dev/null || die "neither rpmbuild nor docker is available"
        msg "no host rpmbuild; using $FEDORA_IMG"
        # The rpmbuild line is a single-quoted heredoc-free string on purpose: the --define
        # argument is one word containing a space, and re-expanding it through an array lost the
        # quoting ("Macro %_topdir has empty body", which reads like a spec error rather than a
        # shell one).
        docker run --rm -v "$rpmroot:/rpmbuild" -e PKG="$PKG" "$FEDORA_IMG" bash -c '
            set -e
            dnf install -y -q rpm-build >/dev/null 2>&1
            rpmbuild --define "_topdir /rpmbuild" --target aarch64 \
                     -bb "/rpmbuild/SPECS/$PKG.spec"
        ' >/dev/null || die "rpmbuild failed"
    fi

    local rpm
    rpm=$(find "$rpmroot/RPMS" -name '*.rpm' | head -n1)
    [ -n "$rpm" ] || die "rpmbuild produced no rpm"
    cp "$rpm" "$OUTDIR/"
    msg "wrote $(basename "$rpm")"
}

case "$WHAT" in
deb)  build_deb ;;
rpm)  build_rpm ;;
both) build_deb; build_rpm ;;
*)    die "usage: $0 [deb|rpm|both]" ;;
esac
