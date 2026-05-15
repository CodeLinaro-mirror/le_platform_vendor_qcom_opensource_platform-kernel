# SPDX-License-Identifier: GPL-2.0
#
# Makefile for drivers/soc/qcom

obj-y += hab/

obj-$(CONFIG_QCOM_WDT_CORE)         += qcom_wdt_core.o
obj-$(CONFIG_QCOM_SOC_WATCHDOG)     += qcom_soc_wdt.o

obj-$(CONFIG_QCOM_MINIDUMP)         += minidump.o
minidump-y                          += msm_minidump.o minidump_log.o
minidump-$(CONFIG_QCOM_MINIDUMP_RM) += minidump_rm.o

obj-$(CONFIG_QCOM_DEBUG_SYMBOL)     += debug_symbol.o
