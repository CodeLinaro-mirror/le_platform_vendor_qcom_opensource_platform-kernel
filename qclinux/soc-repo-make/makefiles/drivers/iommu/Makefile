# SPDX-License-Identifier: GPL-2.0
#
# Makefile for Qualcomm IOMMU drivers

obj-$(CONFIG_ARM_SMMU)           += arm/arm-smmu/
obj-$(CONFIG_QTI_IOMMU_SUPPORT)  += iommu-logger.o
obj-$(CONFIG_QCOM_IOMMU_UTIL)    += qcom_iommu_util.o
qcom_iommu_util-y                += qcom-iommu-util.o
qcom_iommu_util-$(CONFIG_IOMMU_IO_PGTABLE_FAST) += qcom-dma-iommu-generic.o \
                                                    io-pgtable-fast.o \
                                                    dma-mapping-fast.o
qcom_iommu_util-$(CONFIG_IOMMU_IO_PGTABLE_LPAE) += qcom-io-pgtable-arm.o qcom-io-pgtable-alloc.o
