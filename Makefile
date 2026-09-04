# Top-level single-pass build: gunyah_guest first (its Module.symvers resolves
# the mem_accept exports), then the patched virtio-gpu driver against it, then
# the virtio-media driver (self-contained: no gunyah_guest symbols, the
# media_guest pool is static in v1).
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C gunyah_guest KDIR=$(KDIR)
	$(MAKE) -C virtio_gpu KDIR=$(KDIR)
	$(MAKE) -C virtio_media KDIR=$(KDIR)

clean:
	$(MAKE) -C gunyah_guest clean KDIR=$(KDIR)
	$(MAKE) -C virtio_gpu clean KDIR=$(KDIR)
	$(MAKE) -C virtio_media clean KDIR=$(KDIR)
