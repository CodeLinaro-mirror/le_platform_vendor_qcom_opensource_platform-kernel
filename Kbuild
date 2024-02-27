# SPDX-License-Identifier: GPL-2.0-only

ifeq (y, $(findstring y, $(CONFIG_ARCH_SA8155) $(CONFIG_ARCH_SA8195)))
  include $(PLAT_DRV_ROOT)/config/augen3soc.conf
  LINUX_INC += -include $(PLAT_DRV_ROOT)/config/augen3socconf.h
endif

ifeq (y, $(findstring y, $(CONFIG_QTI_QUIN_GVM)))
  include $(PLAT_DRV_ROOT)/config/gvmsoc.conf
  LINUX_INC += -include $(PLAT_DRV_ROOT)/config/gvmsocconf.h
endif

ifeq (y, $(findstring y, $(CONFIG_ARCH_QTI_VM)))
  include $(PLAT_DRV_ROOT)/config/ghgvmsoc.conf
  LINUX_INC += -include $(PLAT_DRV_ROOT)/config/ghgvmsocconf.h
endif

ifeq (y, $(findstring y, $(CONFIG_ARCH_DIREWOLF) $(CONFIG_ARCH_LEMANS) $(CONFIG_ARCH_MONACO_AUTO)))
  include $(PLAT_DRV_ROOT)/config/augen4soc.conf
  LINUX_INC += -include $(PLAT_DRV_ROOT)/config/augen4socconf.h
endif

ifeq ($(CONFIG_ARCH_SA6155), y)
  include $(PLAT_DRV_ROOT)/config/sdmsteppesoc.conf
  LINUX_INC += -include $(PLAT_DRV_ROOT)/config/sdmsteppesocconf.h
endif

ccflags-y += $(LINUX_INC)

LINUXINCLUDE	+= \
		   -I$(PLAT_DRV_ROOT)/include \
		   -I$(PLAT_DRV_ROOT)/include/linux

USERINCLUDE	+= -I$(PLAT_DRV_ROOT)/include/uapi


obj-y  += drivers/
