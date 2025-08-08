// SPDX-License-Identifier: GPL-2.0
// Property of Morat Engine

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/cpumask.h>

static unsigned int poll_ms = 30000;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Polling interval in ms");

static unsigned int load_window_ms = 5000;
module_param(load_window_ms, uint, 0644);
MODULE_PARM_DESC(load_window_ms, "CPU load measurement window in ms");

static unsigned int load_threshold_pct = 15;
module_param(load_threshold_pct, uint, 0644);
MODULE_PARM_DESC(load_threshold_pct, "CPU load threshold percent to trigger restore");

static unsigned int temp_threshold = 42;
module_param(temp_threshold, uint, 0644);
MODULE_PARM_DESC(temp_threshold, "Temperature threshold (C)");

static bool require_temp_read = true;
module_param(require_temp_read, bool, 0644);
MODULE_PARM_DESC(require_temp_read, "Require battery temp read to restore");

static unsigned int hysteresis_short_sec = 60;
module_param(hysteresis_short_sec, uint, 0644);
MODULE_PARM_DESC(hysteresis_short_sec, "Short hysteresis in seconds");

static unsigned int hysteresis_long_sec = 180;
module_param(hysteresis_long_sec, uint, 0644);
MODULE_PARM_DESC(hysteresis_long_sec, "Long hysteresis in seconds");

static unsigned int hysteresis_delta = 4;
module_param(hysteresis_delta, uint, 0644);
MODULE_PARM_DESC(hysteresis_delta, "Temperature delta for hysteresis switch");

static struct task_struct *thermal_task;
static unsigned long last_restore_jiffies;

static long read_file_val(const char *path)
{
	struct file *f;
	char buf[32];
	loff_t pos = 0;
	long val = -1;
	int ret;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return -1;

	ret = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);

	if (ret > 0) {
		buf[ret] = '\0';
		if (kstrtol(buf, 10, &val) == 0)
			return val;
	}

	return -1;
}

static void write_file_val(const char *path, long val)
{
	struct file *f;
	char buf[32];
	int len;
	loff_t pos = 0;

	len = snprintf(buf, sizeof(buf), "%ld\n", val);

	f = filp_open(path, O_WRONLY, 0);
	if (!IS_ERR(f)) {
		kernel_write(f, buf, len, &pos);
		filp_close(f, NULL);
	}
}

static int read_batt_temp(void)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	psy = power_supply_get_by_name("battery");
	if (!psy)
		return -1;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &val);
	if (ret < 0)
		return -1;

	return val.intval / 10; // m°C to °C
}

static unsigned int get_cpu_util(unsigned int window_ms)
{
	unsigned long long u1, n1, s1, i1, io1, irq1, sirq1;
	unsigned long long u2, n2, s2, i2, io2, irq2, sirq2;
	unsigned long long t1, t2, idle1, idle2;
	char buf[128];
	struct file *f;
	loff_t pos;
	int ret;
	unsigned int util = 0;

#define PARSE_CPU_STAT(U,N,S,I,IO,IRQ,SIRQ) \
	sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu", \
		&U, &N, &S, &I, &IO, &IRQ, &SIRQ)

	// First read
	pos = 0;
	f = filp_open("/proc/stat", O_RDONLY, 0);
	if (IS_ERR(f))
		return 0;

	ret = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (ret <= 0)
		return 0;

	buf[ret] = '\0';
	PARSE_CPU_STAT(u1, n1, s1, i1, io1, irq1, sirq1);
	t1 = u1 + n1 + s1 + i1 + io1 + irq1 + sirq1;
	idle1 = i1 + io1;

	// Sleep for window
	msleep(window_ms);

	// Second read
	pos = 0;
	f = filp_open("/proc/stat", O_RDONLY, 0);
	if (IS_ERR(f))
		return 0;

	ret = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (ret <= 0)
		return 0;

	buf[ret] = '\0';
	PARSE_CPU_STAT(u2, n2, s2, i2, io2, irq2, sirq2);
	t2 = u2 + n2 + s2 + i2 + io2 + irq2 + sirq2;
	idle2 = i2 + io2;

	if (t2 > t1) {
		unsigned long long dt = t2 - t1;
		unsigned long long di = idle2 - idle1;
		util = (unsigned int)((dt - di) * 100 / dt);
	}

	return util;
}

static int exotic_thermal_thread(void *data)
{
	last_restore_jiffies = 0;

	while (!kthread_should_stop()) {
		unsigned int hysteresis_now;
		int temp = read_batt_temp();

		if (require_temp_read && temp < 0)
			goto sleep_loop;

		// Pilih hysteresis berdasarkan suhu
		if (temp >= 0 && temp >= temp_threshold - hysteresis_delta)
			hysteresis_now = hysteresis_long_sec;
		else
			hysteresis_now = hysteresis_short_sec;

		if (time_before(jiffies, last_restore_jiffies +
				msecs_to_jiffies(hysteresis_now * 1000)))
			goto sleep_loop;

		if (temp >= 0 && temp >= temp_threshold)
			goto sleep_loop;

		if (get_cpu_util(load_window_ms) < load_threshold_pct)
			goto sleep_loop;

		// Restore max_freq tiap CPU online
		{
			int cpu;
			for_each_online_cpu(cpu) {
				char path_info[128], path_scaling[128];
				long def_khz, cur_max;

				snprintf(path_info, sizeof(path_info),
					 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
				snprintf(path_scaling, sizeof(path_scaling),
					 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);

				def_khz = read_file_val(path_info);
				cur_max = read_file_val(path_scaling);

				if (def_khz > 0 && cur_max > 0 && cur_max < def_khz) {
					write_file_val(path_scaling, def_khz);
				}
			}
		}

		last_restore_jiffies = jiffies;

sleep_loop:
		msleep(poll_ms);
	}
	return 0;
}

static int __init exotic_thermal_init(void)
{
	thermal_task = kthread_run(exotic_thermal_thread, NULL, "exotic_thermal");
	return 0;
}

static void __exit exotic_thermal_exit(void)
{
	if (thermal_task)
		kthread_stop(thermal_task);
}

module_init(exotic_thermal_init);
module_exit(exotic_thermal_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Morat Engine");
MODULE_DESCRIPTION("ExoticThermal Adaptive - Restore big core max freq safely with adaptive hysteresis");