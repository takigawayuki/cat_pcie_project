CONFIG_MODULE_SIG=n

ifneq ($(KERNELRELEASE),)
obj-m := colorbar_pcie_driver.o
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
endif
