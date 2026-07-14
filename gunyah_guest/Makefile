obj-m += gunyah_guest.o
ccflags-y += -I$(src) -DCONFIG_GUNYAH_GUEST=1
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
