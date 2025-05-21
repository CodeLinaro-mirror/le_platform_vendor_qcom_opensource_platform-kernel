# Android makefile for display kernel modules
LOCAL_PATH := $(call my-dir)
VRPC_PATH :=$(call my-dir)/drivers/virtual_fastrpc
RSMFE_PATH :=$(call my-dir)/drivers/rsm_fe

include $(LOCAL_PATH)/drivers/Android.mk
include $(VRPC_PATH)/Android.mk
include $(RSMFE_PATH)/Android.mk