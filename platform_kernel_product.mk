# SPDX-License-Identifier: GPL-2.0-only
ifneq ($(TARGET_USES_GY), true)
ifeq ($(call is-board-platform-in-list,sdmsteppe msmnile gen4), true)
# Drivers for both Metal and GVM
    PRODUCT_PACKAGES += wallpower_charger.ko

ifeq (,$(filter msmnile_gvmq gen4_gvm gen4_hgy, $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX)))
# Drivers for Metal only
    PRODUCT_PACKAGES += aop-set-ddr.ko
    PRODUCT_PACKAGES += silent_boot.ko
    PRODUCT_PACKAGES += silent-mode-hw-monitoring.ko
    PRODUCT_PACKAGES += dump_boot_log.ko

else
# Drivers for GVM only
    PRODUCT_PACKAGES += socinfo_dt.ko
    PRODUCT_PACKAGES += subsystem_notif_virt.ko
ifeq ($(TARGET_HAS_VIRTIO_FASTRPC), true)
    PRODUCT_PACKAGES += vfastrpc.ko
endif
endif
endif

# Drivers for Talos only
ifeq ($(call is-board-platform-in-list,sm6150), true)
ifeq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), sm6150_gvmq))
    PRODUCT_PACKAGES += adsp_vote_smp2p.ko
endif
endif
endif
