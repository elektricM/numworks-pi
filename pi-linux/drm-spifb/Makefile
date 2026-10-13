obj-m += drm-spifb.o
drm-spifb-y := drm-spifb-core.o
# NEON downscale, built with the kernel's FPU flags (see
# Documentation/core-api/floating-point.rst); the core falls back to C
drm-spifb-$(CONFIG_KERNEL_MODE_NEON) += drm-spifb-neon.o
CFLAGS_drm-spifb-neon.o += $(CC_FLAGS_FPU)
CFLAGS_REMOVE_drm-spifb-neon.o += $(CC_FLAGS_NO_FPU)

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all install clean
