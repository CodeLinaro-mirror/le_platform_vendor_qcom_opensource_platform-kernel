AOP_SET_DDR_SELECT := CONFIG_QCOM_AOP_SET_DDR=m
WALLPOWER_CHARGER_SELECT := CONFIG_WALLPOWER_CHARGER=m
SILENT_MODE_SELECT := CONFIG_SILENT_MODE=m
PM_SILENT_MODE_SELECT := CONFIG_PM_SILENT_MODE=m
DUMP_BOOT_LOG_SELECT := CONFIG_DUMP_XBL_LOG=m
SOCINFO_DT_SELECT := CONFIG_QCOM_SOCINFO_DT=m
SUBSYSTEM_NOTIF_VIRT_SELECT := CONFIG_MSM_QUIN_SUBSYSTEM_NOTIF_VIRT=m
VIRTIO_SSR_SELECT := CONFIG_VIRTIO_SSR=m
QCOM_ADSP_VOTE_SMP2P_SELECT := CONFIG_QCOM_ADSP_VOTE_SMP2P=m
CPUFREQ_VM_SELECT := CONFIG_CPUFREQ_VM=m
MSM_S2R_WAKEUP_MARKER := CONFIG_MSM_S2R_WAKEUP_MARKER=m
MSM_BOOT_MARKER := CONFIG_MSM_BOOT_MARKER=m
QCOM_SUBSYS_STATUS := CONFIG_QCOM_SUBSYS_STATUS=m

LOCAL_PATH := $(call my-dir)
LOCAL_MODULE_DDK_BUILD := true
LOCAL_MODULE_DDK_ALLOW_UNSAFE_HEADERS := true
include $(CLEAR_VARS)

# This makefile is only for DLKM
ifneq ($(findstring vendor,$(LOCAL_PATH)),)

ifneq ($(findstring opensource,$(LOCAL_PATH)),)
	SOC_BLD_DIR := $(TOP)/vendor/qcom/opensource/platform-kernel
endif # opensource

DLKM_DIR := $(TOP)/device/qcom/common/dlkm

LOCAL_ADDITIONAL_DEPENDENCIES := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)

###########################################################
# This is set once per LOCAL_PATH, not per (kernel) module
KBUILD_OPTIONS := PLAT_DRV_ROOT=$(SOC_BLD_DIR)
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

# Drivers for both LA-Metal and LA-GVM
##########################################################
KBUILD_OPTIONS += MODNAME=wallpower-charger
KBUILD_OPTIONS += $(WALLPOWER_CHARGER_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,wallpower_charger_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := wallpower_charger.ko
LOCAL_MODULE_KBUILD_NAME  := wallpower_charger.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += wallpower_charger_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,wallpower_charger_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
KBUILD_OPTIONS += MODNAME=boot_marker
KBUILD_OPTIONS += $(MSM_BOOT_MARKER)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,boot_marker_select-module-symvers)/Module.symvers
endif

include $(CLEAR_VARS)

LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := boot_marker.ko
LOCAL_MODULE_KBUILD_NAME  := boot_marker.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += boot_marker_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,boot_marker_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
ifneq ( ,$(filter $(MSMSTEPPE)_au msmnile_au gen4_au, $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX)))
# Drivers for only LA-Metal
###########################################################
KBUILD_OPTIONS += MODNAME=aop_set_ddr_freq
KBUILD_OPTIONS += $(AOP_SET_DDR_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,aop_ddr_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := aop-set-ddr.ko
LOCAL_MODULE_KBUILD_NAME  := aop-set-ddr.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    := aop_ddr_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,aop_ddr_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
KBUILD_OPTIONS += MODNAME=pm-slient-mode
KBUILD_OPTIONS += $(PM_SILENT_MODE_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,pm_silent_mode_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := silent_boot.ko
LOCAL_MODULE_KBUILD_NAME  := silent_boot.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += pm_silent_mode_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,pm_silent_mode_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

##########################################################
KBUILD_OPTIONS += MODNAME=silent-mode
KBUILD_OPTIONS += $(SILENT_MODE_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,silent_mode_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := silent-mode-hw-monitoring.ko
LOCAL_MODULE_KBUILD_NAME  := silent-mode-hw-monitoring.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += silent_mode_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,silent_mode_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
KBUILD_OPTIONS += MODNAME=dump_xbl_log
KBUILD_OPTIONS += $(DUMP_BOOT_LOG_SELECT)

KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,dump_boot_log_select-module-symvers)/Module.symvers

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := dump_boot_log.ko
LOCAL_MODULE_KBUILD_NAME  := dump_boot_log.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += dump_boot_log_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,dump_boot_log_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
KBUILD_OPTIONS += MODNAME=qcom_s2r_wakeup_marker
KBUILD_OPTIONS += $(MSM_S2R_WAKEUP_MARKER)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,qcom_s2r_wakeup_marker_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := s2r_wakeup_marker.ko
LOCAL_MODULE_KBUILD_NAME  := s2r_wakeup_marker.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += qcom_s2r_wakeup_marker_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,qcom_s2r_wakeup_marker_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
KBUILD_OPTIONS += MODNAME=subsystem_status
KBUILD_OPTIONS += $(QCOM_SUBSYS_STATUS)
###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := subsystem_status.ko
LOCAL_MODULE_KBUILD_NAME  := subsystem_status.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

include $(DLKM_DIR)/Build_external_kernelmodule.mk

endif

# Drivers for LA-GVM only
###########################################################
ifneq (,$(filter msmnile_gvmq gen4_gvm gen4_hgy gen5_gvm_gy gen5_gvm gen5_gvm_sgt, $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX)))
KBUILD_OPTIONS += MODNAME=qcom-dt-socinfo
KBUILD_OPTIONS += $(SOCINFO_DT_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,socinfo_dt_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := socinfo_dt.ko
LOCAL_MODULE_KBUILD_NAME  := socinfo_dt.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += socinfo_dt_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,socinfo_dt_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

##########################################################
KBUILD_OPTIONS += MODNAME=vm-cpufreq
KBUILD_OPTIONS += $(CPUFREQ_VM_SELECT)
ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,cpufreq_vm_select-module-symvers)/Module.symvers
endif
##########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := vm-cpufreq.ko
LOCAL_MODULE_KBUILD_NAME  := vm-cpufreq.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)
ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += cpufreq_vm_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,cpufreq_vm_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

##########################################################
KBUILD_OPTIONS += MODNAME=virt-ssr
KBUILD_OPTIONS += $(SUBSYSTEM_NOTIF_VIRT_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,subsystem_notif_virt_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := subsystem_notif_virt.ko
LOCAL_MODULE_KBUILD_NAME  := subsystem_notif_virt.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += subsystem_notif_virt_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,subsystem_notif_virt_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk

##########################################################
KBUILD_OPTIONS += MODNAME=virtio-ssr
KBUILD_OPTIONS += $(VIRTIO_SSR)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,virtio_ssr_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := virtio_ssr.ko
LOCAL_MODULE_KBUILD_NAME  := virtio_ssr.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += virtio_ssr_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,virtio_ssr_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk
endif

###########################################################
ifeq ($(call is-board-platform-in-list,sm6150), true)
KBUILD_OPTIONS += MODNAME=qcom_adsp_vote_smp2p
KBUILD_OPTIONS += $(QCOM_ADSP_VOTE_SMP2P_SELECT)

ifneq ($(TARGET_BOARD_AUTO),true)
KBUILD_OPTIONS += KBUILD_EXTRA_SYMBOLS+=$(PWD)/$(call intermediates-dir-for,DLKM,qcom_adsp_vote_smp2p_select-module-symvers)/Module.symvers
endif

###########################################################
include $(CLEAR_VARS)
LOCAL_SRC_FILES   := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := adsp_vote_smp2p.ko
LOCAL_MODULE_KBUILD_NAME  := adsp_vote_smp2p.ko
LOCAL_MODULE_TAGS         := optional
LOCAL_MODULE_DEBUG_ENABLE := true
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

ifneq ($(TARGET_BOARD_AUTO),true)
LOCAL_REQUIRED_MODULES    += qcom_adsp_vote_smp2p_select-module-symvers
LOCAL_ADDITIONAL_DEPENDENCIES += $(call intermediates-dir-for,DLKM,qcom_adsp_vote_smp2p_select-module-symvers)/Module.symvers
endif

include $(DLKM_DIR)/Build_external_kernelmodule.mk
endif

###########################################################
include $(CLEAR_VARS)
# For incremental compilation
LOCAL_SRC_FILES           := $(wildcard $(LOCAL_PATH)/**/*) $(wildcard $(LOCAL_PATH)/*)
LOCAL_MODULE              := platform_kernel_select-module-symvers
LOCAL_MODULE_STEM         := Module.symvers
LOCAL_MODULE_KBUILD_NAME  := Module.symvers
LOCAL_MODULE_PATH         := $(KERNEL_MODULES_OUT)

include $(DLKM_DIR)/Build_external_kernelmodule.mk

###########################################################
endif # DLKM check
