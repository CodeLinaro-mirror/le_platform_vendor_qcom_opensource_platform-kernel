# SPDX-License-Identifier: GPL-2.0
#
# Makefile for arm-smmu (v2) and Qualcomm extensions

# Required for tracepoint header re-include via define_trace.h
ccflags-y += -I$(src)

obj-$(CONFIG_ARM_SMMU)               += arm_smmu.o
arm_smmu-objs                        += arm-smmu.o arm-smmu-impl.o
arm_smmu-$(CONFIG_ARM_SMMU_QCOM)     += arm-smmu-qcom.o arm-smmu-qcom-pm.o arm-smmu-debug.o
arm_smmu-$(CONFIG_ARM_SMMU_QCOM_DEBUG) += arm-smmu-qcom-debug.o
