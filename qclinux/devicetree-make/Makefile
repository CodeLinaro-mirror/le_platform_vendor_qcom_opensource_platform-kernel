# SPDX-License-Identifier: GPL-2.0
dtb-y += sa8797p-gunyah-vm-lv-qam.dtb
dtb-y += sa8797p-gunyah-vm-lv-qam-overlay.dtbo

KBUILD_OPTIONS += KBUILD_EXTMOD_DTS=.
KBUILD_OPTIONS += dtstree=$(M)
KBUILD_OPTIONS += KBUILD_EXTMOD=

all: dtbs

dtbs:
	$(MAKE) -C $(KERNEL_SRC) dtbs $(KBUILD_OPTIONS)

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)
