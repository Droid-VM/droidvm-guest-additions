# gunyah_guest_mod

Guest-side kernel modules for DroidVM's GuestAccept memory sharing, built
against the Ubuntu guest kernel (7.0.0-27-generic).

- **gunyah_guest/** — `gunyah_guest.ko`: raw HVC `mem_accept` so the guest
  accepts host-SHARE'd RM memparcels into its own stage-2 at the BAR GPA
  (pairs with host `gunyah_host_mod/GKI6.6`). Ships the `linux/gunyah_guest.h`
  UAPI it exports.
- **virtio_gpu/** — patched in-tree `virtio-gpu` driver: calls gunyah_guest's
  mem_accept on HOST3D blob map so shared blobs are backed in the guest
  (stock virtio_gpu SIGBUSes — no accept). Includes the patched
  `linux/gunyah_guest.h` + `uapi/linux/virtio_gpu.h` it builds against.

Build (on guest): `make -C <module> KDIR=/lib/modules/$(uname -r)/build`.
Load order: `virtio_dma_buf`, `gunyah_guest`, then `virtio-gpu`.
See host counterpart Droid-VM/gunyah_host_mod and Droid-VM/droidvm-3d-accel.

## DKMS

Install as a DKMS package (auto-rebuilds on guest kernel upgrades):

    sudo cp -r . /usr/src/droidvm-guest-additions-1.0
    sudo dkms install droidvm-guest-additions/1.0
    sudo update-initramfs -u        # Debian/Ubuntu (dracut -f on Fedora)

`dkms.conf` here is reconstructed from the in-guest install and pending
re-verification on a live guest. `BUILD_EXCLUSIVE_KERNEL` currently bounds
builds to 7.x kernels; multi-series support (per-series pristine virtio-gpu
bases + the ~100-line droidvm patch, selected by `uname -r` in PRE_BUILD) is
planned — see Droid-VM/droidvm-3d-accel guest-patches/linux for the raw
kernel patches.
