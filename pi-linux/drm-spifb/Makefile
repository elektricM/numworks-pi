obj-m += drm-spifb.o
drm-spifb-objs := drm-spifb-core.o drm-spifb-neon.o

# NEON file needs ARM SIMD flags (kernel uses soft-float ABI)
CFLAGS_drm-spifb-neon.o += -mfpu=neon -mfloat-abi=softfp

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all install clean
