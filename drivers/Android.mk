AOP_SET_DDR_SELECT := CONFIG_QCOM_AOP_SET_DDR=m

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

endif

###########################################################
endif # DLKM check
