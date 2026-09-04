#!/bin/bash
# Vendor the virtio-media guest driver from the Droid-VM/virtio-media fork.
#
#   ./sync-virtio-media.sh [path/to/virtio-media/driver]
#
# The default source is ../crosvm_build/external/virtio-media/driver, which is where the meta
# repo's 1_build_crosvm_prepare.sh checks the fork out next to this repo. Only what the module
# build needs is copied -- the .c/.h sources, Kbuild and Makefile; no Bazel or editor files --
# and the fork commit the copy came from is written to virtio_media/SOURCE_COMMIT, so a
# `dkms status` on a guest can be traced back to the fork's history. A copy taken from a fork
# checkout with uncommitted changes under driver/ is marked +dirty there.
#
# The driver is vendored rather than fetched at package build time for the same reason the
# virtio-gpu sources are: the package ships full sources and a guest must be able to rebuild
# from /usr/src alone.
set -euo pipefail
cd "$(dirname "$0")"

SRC=${1:-../crosvm_build/external/virtio-media/driver}
DST=virtio_media

die() { echo "error: $*" >&2; exit 1; }

[ -f "$SRC/Kbuild" ] || die "no Kbuild in $SRC -- point me at the fork's driver/ directory"

mkdir -p "$DST"
rm -f "$DST"/*.c "$DST"/*.h "$DST"/Kbuild "$DST"/Makefile
cp "$SRC"/*.c "$SRC"/*.h "$SRC"/Kbuild "$SRC"/Makefile "$DST"/

commit=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)
if [ "$commit" != unknown ] && ! git -C "$SRC" diff --quiet HEAD -- . 2>/dev/null; then
	commit="$commit+dirty"
fi
# The meta repo checks the fork out as a repo(1) project, whose remote is not called origin.
remote=$(git -C "$SRC" remote 2>/dev/null | grep -x origin || git -C "$SRC" remote 2>/dev/null | head -n1)
url=$(git -C "$SRC" remote get-url "${remote:-origin}" 2>/dev/null || echo unknown)
{
	echo "$commit"
	echo "# Droid-VM/virtio-media driver/ at the commit on the first line ($url)."
	echo "# Regenerate with ./sync-virtio-media.sh; do not edit virtio_media/ by hand."
} > "$DST/SOURCE_COMMIT"

echo "==> $DST synced from $SRC at $commit"
ls "$DST"
