# SPDX-License-Identifier: GPL-2.0
#
# Makefile for hab

# Add include path for trace headers
CFLAGS_hab.o := -I$(src)

# hab_virtio.c is the implementation of CONFIG_MSM_VIRTIO_HAB; define the macro
# so that hab_virtio.h takes the #ifdef branch (declarations only, no stubs).
# Also needed by hab_os.h for msm_timer_get_sclk_ticks().
ccflags-y += -DCONFIG_MSM_VIRTIO_HAB

obj-$(CONFIG_MSM_HAB) += msm_hab.o
msm_hab-y := hab.o \
	hab_msg.o \
	hab_vchan.o \
	hab_pchan.o \
	hab_open.o \
	hab_mimex.o \
	hab_pipe.o \
	hab_parser.o \
	hab_stat.o \
	khab.o \
	hab_linux.o \
	hab_mem_linux.o \
	khab_test.o \
    hab_virtio.o