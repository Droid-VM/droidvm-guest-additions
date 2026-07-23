# virtio_gunyah_accept: host->guest transport for VmAccept::Sync.
# Links against gunyah_guest.ko's exports (build gunyah_guest first).
obj-m += virtio_gunyah_accept.o

ccflags-y += -I$(src) -DCONFIG_GUNYAH_GUEST=1
KBUILD_EXTRA_SYMBOLS := $(src)/../gunyah_guest/Module.symvers

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
