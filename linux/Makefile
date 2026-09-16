# Makefile for the BARROT BRLink Bluetooth USB driver (brtusb)
#
# Build (out of tree, gcc):
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
# Build with clang/LLVM toolchain (CI default):
#   make LLVM=1 -C /lib/modules/$(uname -r)/build M=$(pwd) modules
# Install:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules_install
#   depmod -a
#   modprobe brtusb

obj-m += brtusb.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a
