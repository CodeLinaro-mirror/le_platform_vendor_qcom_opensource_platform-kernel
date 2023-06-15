# SPDX-License-Identifier: GPL-2.0-only

ifeq ($(call is-board-platform-in-list,msmnile), true)
ifeq (,$(filter $(TARGET_BOARD_PLATFORM)$(TARGET_BOARD_SUFFIX), msmnile_gvmq))
    PRODUCT_PACKAGES += aop-set-ddr.ko
    PRODUCT_PACKAGES += wallpower_charger.ko
endif
endif
