// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022, 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/uaccess.h>
#include <soc/qcom/qcom_stats.h>
#include <linux/hashtable.h>

#define TIMER_KHZ 32768
#define MSM_ARCH_TIMER_FREQ     19200000

static u64 get_time_in_msec(u64 counter)
{
	counter *= MSEC_PER_SEC;
	do_div(counter, MSM_ARCH_TIMER_FREQ);
	return counter;
}

static void measure_wake_up_time(void)
{
	u64 wake_up_time, deep_sleep_exit_time, current_time;
	char wakeup_marker[50] = {0,};

	current_time = arch_timer_read_counter();
	deep_sleep_exit_time = get_aosd_sleep_exit_time();

	if (deep_sleep_exit_time) {
		wake_up_time = get_time_in_msec(current_time - deep_sleep_exit_time);
		pr_debug("Current= %llu, wakeup=%llu, kpi=%llu msec\n",
				current_time, deep_sleep_exit_time,
				wake_up_time);
		snprintf(wakeup_marker, sizeof(wakeup_marker),
				"M - STR Wakeup : %llu ms", wake_up_time);
		pr_err("boot_kpi: M - STR Wakeup %llu ms ",wake_up_time);
	}
}

/**
 * boot_kpi_pm_notifier() - PM notifier callback function.
 * @nb:		Pointer to the notifier block.
 * @event:	Suspend state event from PM module.
 * @unused:	Null pointer from PM module.
 *
 * This function is register as callback function to get notifications
 * from the PM module on the system suspend state.
 */
static int boot_kpi_pm_notifier(struct notifier_block *nb,
				  unsigned long event, void *unused)
{
	if (event == PM_POST_SUSPEND)
		measure_wake_up_time();
	return NOTIFY_DONE;
}

static struct notifier_block boot_kpi_pm_nb = {
	.notifier_call = boot_kpi_pm_notifier,
};

static int __init init_bootkpi(void)
{
	int ret;
	pr_info("Starting the STR wakeup module \n");
	ret = register_pm_notifier(&boot_kpi_pm_nb);
	if (ret){
		pr_err("boot_marker: power state notif error\n");
		pr_err("s2r_wakeup: BootKPI init failed %d\n",ret);
	}
	return ret;
}
module_init(init_bootkpi);

MODULE_DESCRIPTION("MSM STR wakeup info driver");
MODULE_LICENSE("GPL v2");
