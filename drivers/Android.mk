AOP_SET_DDR_SELECT := CONFIG_QCOM_AOP_SET_DDR=m
WALLPOWER_CHARGER_SELECT := CONFIG_WALLPOWER_CHARGER=m
SILENT_MODE_SELECT := CONFIG_SILENT_MODE=m
PM_SILENT_MODE_SELECT := CONFIG_PM_SILENT_MODE=m
DUMP_BOOT_LOG_SELECT := CONFIG_DUMP_XBL_LOG=m

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

# This makefile is only for DLKM
ifneq ($(findstring vendor,$(LOCAL_PATH)),)

ifneq ($(findstring opensource,$(LOCAL_PATH)),)
	SOC_BLD_DIR := $(TOP)/vendor/qcom/opensource/platform-kernel
endif # opensource

DLKM_DIR := $(TOP)/device/qcom/common/dlkm

LOCAL_ADDITIONAL_DEPENDENCIES := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)

###########################################################
# This is set once per LOCAL_PATH, not per (kernel) module
KBUILD_OPTIONS := PLAT_DRV_ROOT=$(SOC_BLD_DIR)
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

# Drivers for both LA-Metal and LA-GVM
##########################################################
KBUILD_OPTIONS += MODNAME=wallpower-charger
KBUILD_OPTIONS += $(WALLPOWER_CHARGER_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,wallpower_charger_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := wallpower_charger.ko
LOCAL_MODULE_KBUILD_NAME  := wallpower_charger.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += wallpower_charger_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,wallpower_charger_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

# Drivers for only LA-Metal
###########################################################
ifneq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), msmnile_au))
KBUILD_OPTIONS += MODNAME=aop_set_ddr_freq
KBUILD_OPTIONS += $(AOP_SET_DDR_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,aop_ddr_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := aop-set-ddr.ko
LOCAL_MODULE_KBUILD_NAME  := aop-set-ddr.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    := aop_ddr_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,aop_ddr_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
KBUILD_OPTIONS += MODNAME=pm-slient-mode
KBUILD_OPTIONS += $(PM_SILENT_MODE_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,pm_silent_mode_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := silent_boot.ko
LOCAL_MODULE_KBUILD_NAME  := silent_boot.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += pm_silent_mode_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,pm_silent_mode_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

##########################################################
KBUILD_OPTIONS += MODNAME=silent-mode
KBUILD_OPTIONS += $(SILENT_MODE_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,silent_mode_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := silent-mode-hw-monitoring.ko
LOCAL_MODULE_KBUILD_NAME  := silent-mode-hw-monitoring.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += silent_mode_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,silent_mode_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
KBUILD_OPTIONS += MODNAME=dump_xbl_log
KBUILD_OPTIONS += $(DUMP_BOOT_LOG_SELECT)

KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,dump_boot_log_select-module-symvers)/Module.symvers

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := dump_boot_log.ko
LOCAL_MODULE_KBUILD_NAME  := dump_boot_log.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += dump_boot_log_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,dump_boot_log_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

endif

###########################################################
endif # DLKM check
