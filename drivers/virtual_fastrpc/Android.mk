DLKM_DIR   := device/qcom/common/dlkm
LOCAL_PATH := $(call my-dir)

VRPC_BLD_DIR := $(abspath .)/vendor/qcom/opensource/platform-kernel/virtual_fastrpc

include $(CLEAR_VARS)
$(info DLKM_DIR = $(DLKM_DIR))
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE      := vfastrpc-module-symvers
LOCAL_MODULE_STEM         := Module.symvers
LOCAL_MODULE_KBUILD_NAME  := Module.symvers
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)
include $(DLKM_DIR)/Build_external_kernelmodule.mk

include $(CLEAR_VARS)
$(info DLKM_DIR = $(DLKM_DIR))
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE      := vfastrpc.ko
LOCAL_MODULE_KBUILD_NAME := vfastrpc.ko
LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
KBUILD_OPTIONS += VRPC_ROOT=$(VRPC_BLD_DIR)
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
include $(DLKM_DIR)/Build_external_kernelmodule.mk

# print out variables
$(info KBUILD_OPTIONS = $(KBUILD_OPTIONS))
$(info intermediates dsp symvers path = $(call intermediates-dir-for,DLKM,vfastrpc-module-symvers))
$(info DLKM_DIR = $(DLKM_DIR))
