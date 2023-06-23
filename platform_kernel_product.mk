# SPDX-License-Identifier: GPL-2.0-only

ifeq ($(call is-board-platform-in-list,sdmsteppe msmnile), true)
# Drivers for both Metal and GVM
    PRODUCT_PACKAGES += wallpower_charger.ko

ifeq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), msmnile_gvmq))
# Drivers for Metal only
    PRODUCT_PACKAGES += aop-set-ddr.ko
    PRODUCT_PACKAGES += silent_boot.ko
    PRODUCT_PACKAGES += silent-mode-hw-monitoring.ko
    PRODUCT_PACKAGES += dump_boot_log.ko

else
# Drivers for GVM only
    PRODUCT_PACKAGES += socinfo_dt.ko

endif

# Drivers for Talos only
ifeq ($(call is-board-platform-in-list,sm6150), true)
ifeq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), sm6150_gvmq))
    PRODUCT_PACKAGES += adsp_vote_smp2p.ko
endif
endif
