# SPDX-License-Identifier: GPL-2.0-only

PLAT_DRV_ROOT=$(ROOTDIR)/vendor/qcom/opensource/platform-kernel
KBUILD_OPTIONS := PLAT_DRV_ROOT=$(PLAT_DRV_ROOT)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)

clean:
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions
