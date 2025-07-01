VIRTIO_RSMFE_SELECT := CONFIG_VIRTIO_RSMFE=m

DLKM_DIR := $(TOP)/device/qcom/common/dlkm
LOCAL_PATH := $(call my-dir)
LOCAL_MODULE_DDK_BUILD := true
LOCAL_MODULE_DDK_ALLOW_UNSAFE_HEADERS := true
RSMFE_BLD_DIR := $(abspath .)/vendor/qcom/opensource/platform-kernel/drivers/rsm_fe

KBUILD_OPTIONS += RSMFE_ROOT=$(RSMFE_BLD_DIR)
KBUILD_OPTIONS += $(VIRTIO_RSMFE_SELECT)
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

# virtio RSM
###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE      := rsm_fe.ko
LOCAL_MODULE_KBUILD_NAME := rsm_fe.ko
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
include $(DLKM_DIR)/Build_external_kernelmodule.mk

########################### Module.symvers ############################
include $(CLEAR_VARS)
LOCAL_SRC_FILES           := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := rsm-fe-module-symvers
LOCAL_MODULE_STEM         := Module.symvers
LOCAL_MODULE_KBUILD_NAME  := Module.symvers
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)
include $(DLKM_DIR)/Build_external_kernelmodule.mk
