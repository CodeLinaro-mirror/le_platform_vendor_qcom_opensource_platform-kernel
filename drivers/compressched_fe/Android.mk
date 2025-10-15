VIRTIO_COMPRESSCHEDFE_SELECT := CONFIG_VIRTIO_COMPRESSCHEDFE=m

DLKM_DIR := $(TOP)/device/qcom/common/dlkm
LOCAL_PATH := $(call my-dir)
LOCAL_MODULE_DDK_BUILD := true
LOCAL_MODULE_DDK_ALLOW_UNSAFE_HEADERS := true
COMPRESSCHEDFE_BLD_DIR := $(abspath .)/vendor/qcom/opensource/platform-kernel/drivers/compressched_fe

KBUILD_OPTIONS += COMPRESSCHEDFE_ROOT=$(COMPRESSCHEDFE_BLD_DIR)
KBUILD_OPTIONS += $(VIRTIO_COMPRESSCHEDFE_SELECT)
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

# virtio COMPRESSCHED
###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE      := compressched_fe.ko
LOCAL_MODULE_KBUILD_NAME := compressched_fe.ko
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
include $(DLKM_DIR)/Build_external_kernelmodule.mk

