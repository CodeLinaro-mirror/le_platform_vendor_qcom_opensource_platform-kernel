// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
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
#include <linux/hashtable.h>

void place_marker(const char *name);
void destroy_marker_kernel(const char *name);
unsigned long long msm_timer_get_sclk_ticks_kernel(void);
static inline int boot_marker_enabled(void) { return 1; }

#define MARKER_STRING_WIDTH 40
#define TS_WHOLE_NUM_WIDTH 8
#define TS_PRECISION_WIDTH 3
/* Field width to consider the spaces, 's' character and \n */
#define TIME_FIELD_MISC 4
#define TIME_FIELD_WIDTH \
	(TS_WHOLE_NUM_WIDTH + TS_PRECISION_WIDTH + TIME_FIELD_MISC)

#define MARKER_TOTAL_LEN (MARKER_STRING_WIDTH + TIME_FIELD_WIDTH)
#define MAX_NUM_MARKERS (PAGE_SIZE * 4 / MARKER_TOTAL_LEN)
#define BOOTKPI_BUF_SIZE (PAGE_SIZE * 4)
#define TIMER_KHZ 32768
#define MSM_ARCH_TIMER_FREQ     19200000

static void __iomem *mpm_counter_base;
static uint32_t mpm_counter_freq;

struct boot_marker {
	char marker_name[MARKER_STRING_WIDTH];
	unsigned long long timer_value;
	struct list_head list;
	struct hlist_node hash;
	spinlock_t slock;
};

static struct boot_marker boot_marker_list;
static struct kobject *bootkpi_obj;
static int num_markers;
static DECLARE_HASHTABLE(marker_htable, 5);

unsigned long long msm_timer_get_sclk_ticks_kernel(void)
{
	unsigned long long t1, t2;
	int loop_count = 10;
	int loop_zero_count = 3;
	u64 tmp = USEC_PER_SEC;
	void __iomem *sclk_tick;

	do_div(tmp, TIMER_KHZ);
	tmp /= (loop_zero_count-1);
	sclk_tick = mpm_counter_base;
	if (!sclk_tick)
		return -EINVAL;

	while (loop_zero_count--) {
		t1 = readl_relaxed(sclk_tick);
		do {
			udelay(1);
			t2 = t1;
			t1 = readl_relaxed(sclk_tick);
		} while ((t2 != t1) && --loop_count);
		if (!loop_count) {
			pr_err("boot_marker: SCLK  did not stabilize\n");
			return 0;
		}
		if (t1)
			break;

		udelay(tmp);
	}
	if (!loop_zero_count) {
		pr_err("boot_marker: SCLK reads zero\n");
		return 0;
	}
	return t1;
}
EXPORT_SYMBOL(msm_timer_get_sclk_ticks_kernel);

static void _destroy_boot_marker(const char *name)
{
	struct boot_marker *marker;
	struct boot_marker *temp_addr;

	spin_lock(&boot_marker_list.slock);
	list_for_each_entry_safe(marker, temp_addr, &boot_marker_list.list,
			list) {
		if (strnstr(marker->marker_name, name,
			 strlen(marker->marker_name))) {
			num_markers--;
			hash_del(&marker->hash);
			list_del(&marker->list);
			kfree(marker);
		}
	}
	spin_unlock(&boot_marker_list.slock);
}

/*
 * Function to calculate the cumulative sum of all
 * the characters in the string
 */
static unsigned int calculate_marker_charsum(const char *name)
{
	unsigned int sum = 0;
	int len = strlen(name);

	do {
		sum += (unsigned int)name[--len];
	} while (len);

	return sum;
}

static struct boot_marker *find_entry(const char *name)
{
	struct boot_marker *marker;
	unsigned int sum = calculate_marker_charsum(name);

	hash_for_each_possible(marker_htable, marker, hash, sum) {
		if (!strcmp(marker->marker_name, name))
			return marker;
	}

	return NULL;
}

static void _create_boot_marker(const char *name,
		unsigned long long timer_value)
{
	struct boot_marker *new_boot_marker;
	struct boot_marker *marker;
	unsigned int sum;

	if (num_markers >= MAX_NUM_MARKERS) {
		pr_err("boot_marker: Cannot create marker %s. Limit exceeded!\n",
			name);
		return;
	}

	marker = find_entry(name);
	if (marker) {
		marker->timer_value = timer_value;
		return;
	}

	pr_debug("%-*s%*llu.%0*llu seconds\n",
			MARKER_STRING_WIDTH, name,
			TS_WHOLE_NUM_WIDTH, timer_value/TIMER_KHZ,
			TS_PRECISION_WIDTH, ((timer_value % TIMER_KHZ)
			 * 1000) / TIMER_KHZ);

	new_boot_marker = kmalloc(sizeof(*new_boot_marker), GFP_ATOMIC);
	if (!new_boot_marker)
		return;

	strscpy(new_boot_marker->marker_name, name,
			sizeof(new_boot_marker->marker_name));
	new_boot_marker->timer_value = timer_value;
	sum = calculate_marker_charsum(new_boot_marker->marker_name);

	spin_lock(&boot_marker_list.slock);
	list_add_tail(&(new_boot_marker->list), &(boot_marker_list.list));
	hash_add(marker_htable, &new_boot_marker->hash, sum);
	spin_unlock(&boot_marker_list.slock);
	num_markers++;
}

static void boot_marker_cleanup(void)
{
	struct boot_marker *marker;
	struct boot_marker *temp_addr;

	spin_lock(&boot_marker_list.slock);
	list_for_each_entry_safe(marker, temp_addr, &boot_marker_list.list,
			list) {
		num_markers--;
		hash_del(&marker->hash);
		list_del(&marker->list);
		kfree(marker);
	}
	spin_unlock(&boot_marker_list.slock);
}

void place_marker(const char *name)
{
	_create_boot_marker((char *)name, msm_timer_get_sclk_ticks_kernel());
}
EXPORT_SYMBOL(place_marker);

void destroy_marker_kernel(const char *name)
{
	_destroy_boot_marker((char *) name);
}
EXPORT_SYMBOL(destroy_marker_kernel);

static ssize_t bootkpi_reader(struct file *fp, struct kobject *obj,
		struct bin_attribute *bin_attr, char *user_buffer, loff_t off,
		size_t count)
{
	struct boot_marker *marker;
	unsigned long ts_whole_num, ts_precision;
	static char *kpi_buf;
	static int temp;
	int ret = 0;

	if (!kpi_buf) {
		kpi_buf = kmalloc(BOOTKPI_BUF_SIZE, GFP_KERNEL);
		if (!kpi_buf)
			return -ENOMEM;
	}

	if (!temp) {
		spin_lock(&boot_marker_list.slock);
		list_for_each_entry(marker, &boot_marker_list.list, list) {
			WARN_ON((BOOTKPI_BUF_SIZE - temp) <= 0);

			ts_whole_num = marker->timer_value/TIMER_KHZ;
			ts_precision = ((marker->timer_value % TIMER_KHZ)
					* 1000)	/ TIMER_KHZ;

			/*
			 * Field width of
			 * Marker name		- MARKER_STRING_WIDTH
			 * Timestamp		- TS_WHOLE_NUM_WIDTH
			 * Timestamp precision	- TS_PRECISION_WIDTH
			 */
			temp += scnprintf(kpi_buf + temp,
					BOOTKPI_BUF_SIZE - temp,
					"%-*s%*llu.%0*llu s\n",
					MARKER_STRING_WIDTH,
					marker->marker_name,
					TS_WHOLE_NUM_WIDTH, ts_whole_num,
					TS_PRECISION_WIDTH, ts_precision);


		}

		spin_unlock(&boot_marker_list.slock);
	}

	if (temp - off > count)
		ret = scnprintf(user_buffer, count, "%s", kpi_buf + off);
	else
		ret = scnprintf(user_buffer, temp + 1 - off, "%s", kpi_buf + off);

	if (ret == 0) {
		kfree(kpi_buf);
		kpi_buf = NULL;
		temp = 0;
	}
	return ret;
}

static ssize_t bootkpi_writer(struct file *fp, struct kobject *obj,
		struct bin_attribute *bin_attr, char *user_buffer, loff_t off,
		size_t count)
{
	int rc = 0;
	char buf[MARKER_STRING_WIDTH];

	if (count >= MARKER_STRING_WIDTH)
		return -EINVAL;

	rc = scnprintf(buf, sizeof(buf) - 1, "%s", user_buffer);
	if (rc < 0)
		return rc;

	buf[rc] = '\0';
	place_marker(buf);
	return rc;
}

static ssize_t mpm_timer_read(struct kobject *obj, struct kobj_attribute *attr,
		char *user_buffer)
{
	unsigned long long timer_value;
	char buf[100];
	int temp = 0;

	timer_value = msm_timer_get_sclk_ticks_kernel();

	temp = scnprintf(buf, sizeof(buf), "%llu.%03llu seconds\n",
			timer_value/TIMER_KHZ,
			(((timer_value % TIMER_KHZ) * 1000) / TIMER_KHZ));

	return scnprintf(user_buffer, temp + 1, "%s\n", buf);
}

static struct bin_attribute kpi_values_attribute =
	__BIN_ATTR(kpi_values, 0664, bootkpi_reader, bootkpi_writer, 0);

static struct kobj_attribute mpm_timer_attribute =
	__ATTR(mpm_timer, 0444, mpm_timer_read, NULL);

static int bootkpi_sysfs_init(void)
{
	int ret;

	bootkpi_obj = kobject_create_and_add("boot_kpi", kernel_kobj);
	if (!bootkpi_obj) {
		pr_err("boot_marker: Could not create kobject\n");
		ret = -ENOMEM;
		goto kobj_err;
	}

	ret = sysfs_create_file(bootkpi_obj, &mpm_timer_attribute.attr);
	if (ret) {
		pr_err("boot_marker: Could not create sysfs file\n");
		goto err;
	}

	ret = sysfs_create_bin_file(bootkpi_obj, &kpi_values_attribute);
	if (ret) {
		pr_err("boot_marker: Could not create sysfs bin file\n");
		sysfs_remove_file(bootkpi_obj, &mpm_timer_attribute.attr);
	}

	return 0;
err:
	kobject_del(bootkpi_obj);
kobj_err:
	return ret;
}

static int init_bootkpi(void)
{
	int ret = 0;

	ret = bootkpi_sysfs_init();
	if (ret)
		return ret;

	INIT_LIST_HEAD(&boot_marker_list.list);
	spin_lock_init(&boot_marker_list.slock);

	return 0;
}

static void exit_bootkpi(void)
{
	boot_marker_cleanup();
	sysfs_remove_file(bootkpi_obj, &mpm_timer_attribute.attr);
	sysfs_remove_bin_file(bootkpi_obj, &kpi_values_attribute);
	kobject_del(bootkpi_obj);
}

static int mpm_parse_dt(void)
{
	struct device_node *np_mpm2;

	np_mpm2 = of_find_compatible_node(NULL, NULL,
				"qcom,mpm2-sleep-counter");
	if (!np_mpm2) {
		pr_err("mpm_counter: can't find DT node\n");
		goto err1;
	}

	if (of_property_read_u32(np_mpm2, "clock-frequency", &mpm_counter_freq))
		goto err2;

	if (of_get_address(np_mpm2, 0, NULL, NULL)) {
		mpm_counter_base = of_iomap(np_mpm2, 0);
		if (!mpm_counter_base) {
			pr_err("mpm_counter: cant map counter base\n");
			goto err2;
		}
	} else {
		goto err2;
	}

	return 0;

err2:
	of_node_put(np_mpm2);
err1:
	return -ENODEV;
}

static void print_boot_marker(void)
{
	pr_info("KPI: Kernel MPM timestamp = %u\n",
		readl_relaxed(mpm_counter_base));
	pr_info("KPI: Kernel MPM Clock frequency = %u\n",
		mpm_counter_freq);
}

static int __init boot_marker_init(void)
{
	int ret;

	ret = mpm_parse_dt();
	if (ret < 0)
		return -ENODEV;

	print_boot_marker();
	if (boot_marker_enabled()) {
		ret = init_bootkpi();
		if (ret) {
			pr_err("boot_marker: BootKPI init failed\n");
			return ret;
		}
	} else {
		iounmap(mpm_counter_base);
	}

	return 0;
}
module_init(boot_marker_init);

static void __exit boot_marker_exit(void)
{
	if (boot_marker_enabled()) {
		exit_bootkpi();
		iounmap(mpm_counter_base);
	}
}
module_exit(boot_marker_exit)

MODULE_DESCRIPTION("MSM boot marker driver");
MODULE_LICENSE("GPL v2");
