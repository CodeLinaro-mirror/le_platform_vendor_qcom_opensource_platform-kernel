# SPDX-License-Identifier: GPL-2.0-only
ifneq (,$(call is-board-platform-in-list2, $(MSMSTEPPE) msmnile gen4))
# Drivers for both Metal and GVM
    PRODUCT_PACKAGES += wallpower_charger.ko
    PRODUCT_PACKAGES += boot_marker.ko

ifeq (,$(filter msmnile_gvmq msmnile_gvmq_sgt gen4_gvm gen4_gvm_qmaa gen4_gvm_sgt gen4_hgy gen4_gvm_gy gen4_gvm_gy_sgt gen4_gvm_gy_qmaa gen4_gvm_cmu gen4_gvm_vcu, $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX)$(TARGET_BOARD_DERIVATIVE_SUFFIX)))
# Drivers for Metal only
    PRODUCT_PACKAGES += aop-set-ddr.ko
    PRODUCT_PACKAGES += silent_boot.ko
    PRODUCT_PACKAGES += silent-mode-hw-monitoring.ko
    PRODUCT_PACKAGES += dump_boot_log.ko
    PRODUCT_PACKAGES += s2r_wakeup_marker.ko
    PRODUCT_PACKAGES += subsystem_status.ko
    PRODUCT_PACKAGES += mem-online.ko
else
# Drivers for GVM only
    PRODUCT_PACKAGES += socinfo_dt.ko
ifneq ($(TARGET_USES_GY), true)
    PRODUCT_PACKAGES += subsystem_notif_virt.ko
    PRODUCT_PACKAGES += vm-cpufreq.ko
else
    PRODUCT_PACKAGES += virtio_ssr.ko
endif
ifeq ($(TARGET_HAS_VIRTIO_FASTRPC), true)
    PRODUCT_PACKAGES += vfastrpc.ko
endif
ifeq ($(TARGET_HAS_HYBRID_FASTRPC), true)
    PRODUCT_PACKAGES += hfastrpc.ko
endif

ifeq ($(TARGET_HAS_VIRTIO_RSM), true)
    PRODUCT_PACKAGES += rsm_fe.ko
endif

endif
endif

# Drivers for Talos only
ifneq (,$(call is-board-platform-in-list2,sm6150))
ifeq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), sm6150_gvmq))
    PRODUCT_PACKAGES += adsp_vote_smp2p.ko
endif
endif
