#!/bin/bash
# Unit-test packaging/hooks.sh against stubbed dkms / initramfs tools.
#
#   ./tests/test-hooks.sh
#
# WHY STUBS RATHER THAN A CONTAINER
#
# The parts of the install that have actually gone wrong on this project are not "does the module
# compile" -- they are WHICH COMMAND gets run and WITH WHICH ARGUMENTS:
#
#   * `update-initramfs -u` rebuilds the newest installed kernel's initrd, not the running one, so
#     on a guest that has pulled a kernel update the install reports success while the initrd
#     being booted still holds the previous module.
#   * On Fedora the tool is dracut, taking --kver rather than -k.
#   * A build failure must WARN, not fail the transaction -- a failed `dnf install` leaves the
#     user with no package to remove either.
#
# None of those need a kernel to test, and none of them can be tested in a container anyway: a
# container has no /boot, no dracut config, and no headers matching the guest's 7.x kernel. So the
# stubs record their argv and the assertions read it back.
set -u
cd "$(dirname "$0")/.."

PASS=0 FAIL=0
ok()   { PASS=$((PASS+1)); echo "  ok   - $*"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL - $*"; }
check() { # check <description> <expected-substring> <file>
    if grep -qF -- "$2" "$3"; then ok "$1"; else
        bad "$1"; echo "        wanted: $2"; echo "        got:"; sed 's/^/          /' "$3"; fi
}
check_absent() {
    if grep -qF -- "$2" "$3"; then bad "$1 (found '$2')"; else ok "$1"; fi
}

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
BIN=$TMP/bin; mkdir -p "$BIN"

# HERMETIC PATH. Every case runs with PATH=$BIN and nothing else, so "this tool is absent" is
# tested by not creating a stub -- never by deleting one and letting the real tool be found.
#
# That distinction is not academic: an earlier version of this file removed the update-initramfs
# stub to simulate Fedora, and the host's REAL update-initramfs ran and regenerated /boot on the
# build machine. A test for "which command would we run" must not be able to run it.
#
# hooks.sh needs exactly these real utilities; linking them in by absolute path keeps the rest of
# the system out of reach.
for u in uname grep sed sort ls rm; do ln -sf "$(command -v $u)" "$BIN/$u"; done

# ga_install deletes stale dkms trees with `rm -rf`. Those paths are absolute in a real install,
# so the test points them at a scratch tree -- the same reason the stubs exist at all: a test for
# "what would we delete" must not be able to delete the build machine's /usr/src or /var/lib/dkms.
export GA_USR_SRC="$TMP/usr/src"
export GA_DKMS_STATE="$TMP/var/lib/dkms"
mkdir -p "$GA_USR_SRC" "$GA_DKMS_STATE"

# hooks.sh as the package would ship it: placeholders substituted.
VERSTR='1.0+test'
sed -e 's/@PKG@/droidvm-guest-additions/g' -e "s/@VER@/$VERSTR/g" packaging/hooks.sh > "$TMP/hooks.sh"

# stub <name> <exit-code> -- records "name arg arg ..." into $TMP/calls.
stub() {
    cat > "$BIN/$1" <<EOF
#!/bin/sh
echo "$1 \$*" >> "$TMP/calls"
exit ${2:-0}
EOF
    chmod +x "$BIN/$1"
}

run_case() { # run_case <name> ; caller sets up stubs first
    : > "$TMP/calls"
    ( PATH="$BIN"; . "$TMP/hooks.sh"; "$@" ) > "$TMP/out" 2>&1
    cat "$TMP/out" >> "$TMP/calls"
}

echo "== Debian: install, module builds =="
stub dkms 0; stub update-initramfs 0; stub modinfo 0; stub dracut 0
# dkms status must report something for the "already added" branch; simplest is the default stub
# printing nothing, which drives `dkms add`.
run_case ga_install
check "adds the package to dkms"        "dkms add -m droidvm-guest-additions -v 1.0+test" "$TMP/calls"
check "installs it"                     "dkms install -m droidvm-guest-additions -v 1.0+test --force" "$TMP/calls"
# dkms 3.2.2 ignores REMAKE_INITRD, so a successful install has to rebuild the initramfs itself
# or early KMS keeps loading the previous virtio-gpu.
check "rebuilds the initramfs on install" "update-initramfs -u -k $(uname -r)" "$TMP/calls"

echo "== A stray unowned dkms registration is cleaned up =="
# The state a guest is in after the pre-packaging install.sh: dkms knows "1.0", no package owns
# it. Both dkms status formats are exercised because 2.x and 3.x differ in the separator.
for fmt in 'droidvm-guest-additions/1.0, 7.0.0-28-generic, aarch64: installed' \
           'droidvm-guest-additions, 1.0, 7.0.0-28-generic, aarch64: installed'; do
    # Only the UN-versioned enumeration reports the stray; a versioned query for our own
    # version finds nothing, which is the real state of a guest that ran the old install.sh.
    cat > "$BIN/dkms" <<EOF
#!/bin/sh
echo "dkms \$*" >> "$TMP/calls"
[ "\$*" = "status -m droidvm-guest-additions" ] && echo "$fmt"
exit 0
EOF
    chmod +x "$BIN/dkms"
    stub modinfo 0; stub update-initramfs 0
    run_case ga_install
    check "removes the stray 1.0 (${fmt%%,*}...)" "dkms remove -m droidvm-guest-additions -v 1.0 --all" "$TMP/calls"
    check_absent "and leaves its own (unregistered) version alone" "-v 1.0+test --all" "$TMP/calls"
done

echo "== A stale dkms state directory is removed, not just unregistered (defect D4) =="
# What an upgrade of this package left behind: the old version's sources are gone (dpkg deleted
# them with the old package), so `dkms remove` fails with "Missing the module source directory"
# and only the registration line is printed -- while /var/lib/dkms/<pkg>/<old> survives and
# `dkms status` reports it as "broken ... Manual intervention is required!" from then on.
OLD=1.0+droidvm.r13.gcccd9078
mkdir -p "$GA_DKMS_STATE/droidvm-guest-additions/$OLD/build"
: > "$GA_DKMS_STATE/droidvm-guest-additions/$OLD/build/make.log"
ln -sfn "$OLD/7.0.0-30-generic/aarch64" \
        "$GA_DKMS_STATE/droidvm-guest-additions/kernel-7.0.0-30-generic-aarch64"
cat > "$BIN/dkms" <<EOF
#!/bin/sh
echo "dkms \$*" >> "$TMP/calls"
# The broken entry is what `dkms status` reports for a version whose sources are missing; the
# remove that would clear it fails, exactly as it does on the guest.
[ "\$*" = "status -m droidvm-guest-additions" ] && \
    echo "droidvm-guest-additions/$OLD: broken. Missing the module source directory"
case "\$*" in *"remove -m droidvm-guest-additions -v $OLD"*) exit 1 ;; esac
exit 0
EOF
chmod +x "$BIN/dkms"
stub modinfo 0; stub update-initramfs 0
run_case ga_install
check "still unregisters the stale version" \
      "dkms remove -m droidvm-guest-additions -v $OLD --all" "$TMP/calls"
if [ -e "$GA_DKMS_STATE/droidvm-guest-additions/$OLD" ]; then
    bad "leaves $GA_DKMS_STATE/droidvm-guest-additions/$OLD behind (this is defect D4)"
else ok "removes the stale state directory"; fi
if [ -L "$GA_DKMS_STATE/droidvm-guest-additions/kernel-7.0.0-30-generic-aarch64" ]; then
    bad "leaves a dangling kernel-* symlink behind"
else ok "removes the dangling kernel-* symlink"; fi
check_absent "never treats a kernel-* symlink as a version" \
      "-v kernel-7.0.0-30-generic-aarch64" "$TMP/calls"

echo "== A stale state directory with no registration at all is still removed =="
# `dkms status` says nothing (dkms 3.x skips a tree it cannot parse), so enumerating the status
# output alone would never see this one; the directory listing is what finds it.
ORPHAN=1.0+droidvm.r12.gdeadbee
mkdir -p "$GA_DKMS_STATE/droidvm-guest-additions/$ORPHAN"
mkdir -p "$GA_USR_SRC/droidvm-guest-additions-$ORPHAN"
stub dkms 0; stub modinfo 0; stub update-initramfs 0
run_case ga_install
if [ -e "$GA_DKMS_STATE/droidvm-guest-additions/$ORPHAN" ] ||
   [ -e "$GA_USR_SRC/droidvm-guest-additions-$ORPHAN" ]; then
    bad "an unregistered leftover tree survives ga_install"
else ok "removes an unregistered leftover tree"; fi

echo "== Debian: removal rebuilds the initramfs for the RUNNING kernel =="
stub dkms 0; stub update-initramfs 0
# ga_remove only acts when dkms status reports the version, so make it say so.
cat > "$BIN/dkms" <<EOF
#!/bin/sh
echo "dkms \$*" >> "$TMP/calls"
[ "\$1" = status ] && echo "droidvm-guest-additions/1.0+test, $(uname -r), aarch64: installed"
exit 0
EOF
chmod +x "$BIN/dkms"
run_case ga_remove
check "removes the dkms modules"        "dkms remove -m droidvm-guest-additions -v 1.0+test --all" "$TMP/calls"
check "rebuilds initramfs for the running kernel" "update-initramfs -u -k $(uname -r)" "$TMP/calls"
if grep -qxF "update-initramfs -u" "$TMP/calls"; then
    bad "never uses bare -u (that rebuilds the NEWEST kernel's initrd, not the running one)"
else ok "never uses bare -u"; fi

echo "== Fedora: no update-initramfs, so dracut with --kver =="
rm -f "$BIN/update-initramfs"        # never on PATH in this case; PATH is $BIN alone
stub dracut 0
run_case ga_remove
check "uses dracut"                     "dracut -f --kver $(uname -r)" "$TMP/calls"

echo "== Neither tool present: warn, do not die =="
rm -f "$BIN/dracut"
run_case ga_remove
check "warns about the initramfs"       "rebuild the initramfs yourself" "$TMP/calls"

echo "== Build failure must not fail the transaction =="
cat > "$BIN/dkms" <<EOF
#!/bin/sh
echo "dkms \$*" >> "$TMP/calls"
case "\$1" in status) exit 0 ;; add) exit 0 ;; *) exit 1 ;; esac
EOF
chmod +x "$BIN/dkms"
stub modinfo 0
( PATH="$BIN"; . "$TMP/hooks.sh"; ga_install ) > "$TMP/out" 2>&1
rc=$?
[ $rc -eq 0 ] && ok "ga_install returns 0 when dkms install fails" \
              || bad "ga_install returned $rc; a failed build must not abort the package install"
cp "$TMP/out" "$TMP/calls"
check_absent "no initramfs rebuild when nothing was built" "update-initramfs" "$TMP/calls"
case "$(uname -r)" in
7.*) check "names the build log"        "make.log" "$TMP/calls" ;;
*)   check "explains BUILD_EXCLUSIVE_KERNEL" "outside the range dkms.conf supports" "$TMP/calls" ;;
esac

echo "== The displacement check reports what modinfo actually says =="
cat > "$BIN/dkms" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$BIN/dkms"
cat > "$BIN/modinfo" <<'EOF'
#!/bin/sh
echo /lib/modules/7.0.0-28-generic/kernel/drivers/gpu/drm/virtio/virtio-gpu.ko.zst
EOF
chmod +x "$BIN/modinfo"
run_case ga_install
check "warns when virtio_gpu is NOT the dkms copy" "expected .../updates/dkms/" "$TMP/calls"

cat > "$BIN/modinfo" <<'EOF'
#!/bin/sh
echo /lib/modules/7.0.0-28-generic/updates/dkms/virtio-gpu.ko.zst
EOF
chmod +x "$BIN/modinfo"
run_case ga_install
check "confirms when it IS the dkms copy"  "virtio_gpu resolves to /lib/modules" "$TMP/calls"
check_absent "and does not warn then"      "expected .../updates/dkms/" "$TMP/calls"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
