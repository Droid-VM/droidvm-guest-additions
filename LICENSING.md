# Licensing

This repository holds two kinds of material and they are licensed differently.

## Material inherited from upstream

- **the Linux kernel** (GPL-2.0-only) — https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git

Every file that came from an upstream project stays under that project's
license. Nothing here relicenses it, and modifications to those files do not
relicense them either — a patched upstream file is still an upstream file.

## Material written for DroidVM

Files carrying `SPDX-License-Identifier: GPL-2.0-or-later` are DroidVM work
and are licensed under the GNU GPL, version 2 or later, **with the
additional permissions in `ADDITIONAL-PERMISSIONS`**.

Those permissions exist so this work can go upstream. They let anyone
relicense it under the terms an upstream project requires, for the purpose of
getting it merged there — and only for that purpose. Once upstream publishes
it, upstream's license governs that copy.

## Third-party material that is neither

Everything under `virtio_gpu/` is the Linux kernel's virtio-gpu driver with
DroidVM patches applied in place. Those files keep the kernel's own headers and
are GPL-2.0 kernel code, not DroidVM work — a patched upstream file is still an
upstream file. `virtio_gpu/uapi/` and `virtio_gpu/linux/` are copied kernel
headers and keep their own terms.

`gunyah_guest/` and `tests/` are DroidVM-written.

## Contributing

See `CONTRIBUTING.md`. Sign-off is required; there is no CLA.
