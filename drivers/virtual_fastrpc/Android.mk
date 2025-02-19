VIRTIO_FASTRPC_SELECT := CONFIG_VIRTIO_FASTRPC=m
HYBRID_FASTRPC_SELECT := CONFIG_HYBRID_FASTRPC=m

DLKM_DIR := $(TOP)/device/qcom/common/dlkm
LOCAL_PATH := $(call my-dir)
LOCAL_MODULE_DDK_BUILD := true
LOCAL_MODULE_DDK_ALLOW_UNSAFE_HEADERS := true
VRPC_BLD_DIR := $(abspath .)/vendor/qcom/opensource/platform-kernel/drivers/virtual_fastrpc

KBUILD_OPTIONS += VRPC_ROOT=$(VRPC_BLD_DIR)
KBUILD_OPTIONS += $(HYBRID_FASTRPC_SELECT)
KBUILD_OPTIONS += $(VIRTIO_FASTRPC_SELECT)
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

# Hybrid fastrpc
###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE      := hfastrpc.ko
LOCAL_MODULE_KBUILD_NAME := hfastrpc.ko
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
include $(DLKM_DIR)/Build_external_kernelmodule.mk
