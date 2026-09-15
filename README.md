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
- **virtio_media/** — `virtio-media.ko`, the V4L2 guest driver for DroidVM's
  camera and codec devices (host-side: crosvm `--virtio-media`). Finds the
  `media_host` / `media_guest` pools in `/reserved-memory` and allocates the
  buffers of guest-filled queues itself (module parameter
  `driver_owned_queues=output|all|none`, `pool_debug=1` logs every
  allocation). A vendored copy of the Droid-VM/virtio-media fork's `driver/`;
  `virtio_media/SOURCE_COMMIT` names the fork commit and
  `./sync-virtio-media.sh` refreshes it. Do not edit the copy by hand.

Build (on guest): `make -C <module> KDIR=/lib/modules/$(uname -r)/build`.
Load order: `virtio_dma_buf`, `gunyah_guest`, then `virtio-gpu`;
`virtio-media` is independent (it pulls in `videodev`, `videobuf2` and
`drm_buddy` through modprobe).
See host counterpart Droid-VM/gunyah_host_mod and Droid-VM/droidvm-3d-accel.

## Install

One-liner (inside the guest, installs as a DKMS package that auto-rebuilds on
guest kernel upgrades, then refreshes the initramfs):

    curl -L https://raw.githubusercontent.com/Droid-VM/droidvm-guest-additions/wip/3d-accel/install.sh | sudo bash

From a checkout, `sudo ./install.sh` does the same. Optional env vars:
`DROIDVM_GA_REPO`/`DROIDVM_GA_REF` pick the source repo/branch,
`DROIDVM_MESA_URL` additionally installs a guest mesa: a
`mesa-guest-<variant>_<ver>_arm64.deb` from `8_build_guest_mesa_cross.sh` (URL
or local path), or a legacy tarball. Prefer the deb -- the two variants install
to the same prefix and Conflict, so dpkg refuses the second one instead of
silently replacing the first one's libgallium.

`DROIDVM_VA_URL` additionally installs the **VA-API backend**: a
`libva-v4l2_<ver>_arm64.deb` from the meta repo's `10_build_guest_va.sh` (URL
or local path; a `.deb` only, there is no tarball form). It ships
`/usr/lib/aarch64-linux-gnu/dri/v4l2_drv_video.so` plus
`/etc/profile.d/droidvm-va.sh`, which exports `LIBVA_DRIVER_NAME=v4l2` and
`GST_VAAPI_ALL_DRIVERS=1`. Without that first variable libva asks DRM for the
driver's name, gets `virtio_gpu`, looks for a `virtio_gpu_drv_video.so` that
does not exist in this guest, and every VA-API client silently decodes in
software. Installed with `dpkg -i` and then `apt-get -f install`: it is a
standalone file with no repository behind it.

Two things `profile.d` does **not** reach: the shell that ran the installer
(`. /etc/profile.d/droidvm-va.sh` for this session) and systemd services, which
need their own `Environment=LIBVA_DRIVER_NAME=v4l2`. Nothing that speaks V4L2
M2M directly — `ffmpeg -c:v h264_v4l2m2m`, `gst v4l2videodec` — needs this
package or changes behaviour when it is installed.

    DROIDVM_VA_URL=./libva-v4l2_0+droidvm.r367.g74b336a4_arm64.deb sudo ./install.sh

Manual DKMS route (what install.sh automates):

    sudo cp -r . /usr/src/droidvm-guest-additions-1.0
    sudo dkms install droidvm-guest-additions/1.0
    sudo update-initramfs -u        # Debian/Ubuntu (dracut -f on Fedora)

`dkms.conf` here is reconstructed from the in-guest install and pending
re-verification on a live guest. `BUILD_EXCLUSIVE_KERNEL` currently bounds
builds to 7.x kernels; multi-series support (per-series pristine virtio-gpu
bases + the ~100-line droidvm patch, selected by `uname -r` in PRE_BUILD) is
planned — see Droid-VM/droidvm-3d-accel guest-patches/linux for the raw
kernel patches.
