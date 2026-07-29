/* Exercise virtio_gpu_guest_pool_alloc()'s derived min_block_size across sizes that pick
 * different values for it -- including one deliberately not a multiple of that block size,
 * which is the case drm_buddy rejects outright if the size is not re-aligned. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

#ifndef VIRTGPU_BLOB_FLAG_CREATE_GUEST_HANDLE
#define VIRTGPU_BLOB_FLAG_CREATE_GUEST_HANDLE 0x0008
#endif

static int try_size(int fd, unsigned long long size, const char *note)
{
    struct drm_virtgpu_resource_create_blob a;
    memset(&a, 0, sizeof(a));
    a.blob_mem = VIRTGPU_BLOB_MEM_GUEST;
    a.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
    a.size = size;
    int ret = ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &a);
    printf("  %-14s size=%-12llu %s\n", note, size,
           ret ? strerror(errno) : "OK");
    if (!ret) {
        struct drm_gem_close c; memset(&c, 0, sizeof(c)); c.handle = a.bo_handle;
        ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
    }
    return ret;
}

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) { perror("open renderD128"); return 1; }
    int rc = 0;
    rc |= try_size(fd, 4096ULL,                    "4KiB");
    rc |= try_size(fd, 1ULL<<20,                   "1MiB");
    rc |= try_size(fd, 128ULL<<20,                 "128MiB");
    rc |= try_size(fd, (128ULL<<20) + 4096,        "128MiB+4KiB");
    rc |= try_size(fd, 512ULL<<20,                 "512MiB");
    close(fd);
    printf("%s\n", rc ? "RESULT: some sizes FAILED" : "RESULT: all sizes OK");
    return rc ? 1 : 0;
}

/*
 * Build and run inside the guest:
 *   gcc -O1 -Wall -I/usr/include/libdrm tests/gpool_test.c -o gpool_test && ./gpool_test
 *
 * Reaching virtio_gpu_guest_pool_create() needs neither a context nor gfxstream: any
 * BLOB_MEM_GUEST blob routes there once the guest-alloc pool exists, so this works on a VM
 * whose mesa is the kgsl variant.
 *
 * What to look for in dmesg afterwards is the block count, not just the exit status:
 *   virtio-gpu: guest-alloc: scatter allocation in use (2 blocks for 134221824 bytes)
 * Two blocks for 128 MiB+4 KiB is the derived min_block_size working. Hundreds would mean it
 * is not, and the host's UDMABUF_CREATE_LIST would be the next thing to complain -- with an
 * EINVAL that names none of this.
 */
