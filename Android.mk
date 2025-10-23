# Android makefile for display kernel modules
LOCAL_PATH := $(call my-dir)
VRPC_PATH :=$(call my-dir)/drivers/virtual_fastrpc
COMPRESSCHEDFE_PATH :=$(call my-dir)/drivers/compressched_fe

include $(LOCAL_PATH)/drivers/Android.mk
include $(VRPC_PATH)/Android.mk
include $(COMPRESSCHEDFE_PATH)/Android.mk