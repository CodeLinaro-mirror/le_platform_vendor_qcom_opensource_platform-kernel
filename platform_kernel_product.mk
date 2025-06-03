# SPDX-License-Identifier: GPL-2.0-only
ifeq ($(call is-board-platform-in-list, $(MSMSTEPPE) msmnile gen4 gen5), true)
# Drivers for both Metal and GVM
    PRODUCT_PACKAGES += wallpower_charger.ko
    PRODUCT_PACKAGES += boot_marker.ko

ifeq (,$(filter msmnile_gvmq gen4_gvm gen4_hgy gen4_gvm_gy gen5_gvm_gy gen5_gvm, $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX)))
# Drivers for Metal only
    PRODUCT_PACKAGES += aop-set-ddr.ko
    PRODUCT_PACKAGES += silent_boot.ko
    PRODUCT_PACKAGES += silent-mode-hw-monitoring.ko
    PRODUCT_PACKAGES += dump_boot_log.ko
    PRODUCT_PACKAGES += s2r_wakeup_marker.ko
    PRODUCT_PACKAGES += subsystem_status.ko
else
# Drivers for GVM only
    PRODUCT_PACKAGES += socinfo_dt.ko
    PRODUCT_PACKAGES += virtio_ssr.ko
ifneq ($(TARGET_USES_GY), true)
    PRODUCT_PACKAGES += subsystem_notif_virt.ko
    PRODUCT_PACKAGES += vm-cpufreq.ko
endif
ifeq ($(TARGET_HAS_HYBRID_FASTRPC), true)
    PRODUCT_PACKAGES += hfastrpc.ko
endif
endif
endif

# Drivers for Talos only
ifeq ($(call is-board-platform-in-list,sm6150), true)
ifeq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), sm6150_gvmq))
    PRODUCT_PACKAGES += adsp_vote_smp2p.ko
endif
endif
