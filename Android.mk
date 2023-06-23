# Android makefile for display kernel modules
LOCAL_PATH := $(call my-dir)
VRPC_PATH :=$(call my-dir)/drivers/virtual_fastrpc

include $(LOCAL_PATH)/drivers/Android.mk
include $(VRPC_PATH)/Android.mk
