# Top-level single-pass build: gunyah_guest first (its Module.symvers resolves
# the mem_accept exports), then the patched virtio-gpu driver against it.
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C gunyah_guest KDIR=$(KDIR)
	$(MAKE) -C virtio_gpu KDIR=$(KDIR)

clean:
	$(MAKE) -C gunyah_guest clean KDIR=$(KDIR)
	$(MAKE) -C virtio_gpu clean KDIR=$(KDIR)
