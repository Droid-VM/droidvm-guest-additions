obj-m += virtio-gpu.o
virtio-gpu-y := virtgpu_drv.o virtgpu_kms.o virtgpu_gem.o virtgpu_vram.o \
	virtgpu_display.o virtgpu_vq.o virtgpu_fence.o virtgpu_object.o \
	virtgpu_debugfs.o virtgpu_plane.o virtgpu_ioctl.o virtgpu_prime.o \
	virtgpu_trace_points.o virtgpu_submit.o
ccflags-y += -I$(src) -DCONFIG_GUNYAH_GUEST=1
KBUILD_EXTRA_SYMBOLS := $(src)/../gunyah_guest/Module.symvers
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
CFLAGS_virtgpu_trace_points.o := -I$(src)
