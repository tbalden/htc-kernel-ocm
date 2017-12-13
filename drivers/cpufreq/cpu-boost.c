/*
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>

struct cpu_sync {
	int cpu;
	unsigned int input_boost_min;
	unsigned int input_boost_freq;
};

#ifdef CONFIG_HTC_POWER_DEBUG
extern int get_kernel_cluster_info(int *cluster_id, cpumask_t *cluster_cpus);
#else
static int get_kernel_cluster_info(int *cluster_id, cpumask_t *cluster_cpus) { return -1; }
#endif

static DEFINE_PER_CPU(struct cpu_sync, sync_info);
static struct task_struct * up_task[NR_CPUS];
static struct workqueue_struct *cpu_boost_wq;

static bool input_boost_enabled;

static unsigned int input_boost_ms = 200;
module_param(input_boost_ms, uint, 0644);

static bool sched_boost_on_input;
module_param(sched_boost_on_input, bool, 0644);

static bool sched_boost_active;

static int cluster_id[NR_CPUS] = {[0 ... NR_CPUS-1] = -1};
static cpumask_t cluster_cpus[NR_CPUS];
static int cluster_cnt;
static int wake_cluster[NR_CPUS];

static struct delayed_work input_boost_rem;
static u64 last_input_time;

static int set_input_boost_freq(const char *buf, const struct kernel_param *kp)
{
	int i, j, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	bool enabled = false;
	unsigned int boost_freq;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* single number: apply to all CPUs */
	if (!ntokens) {
		if (sscanf(buf, "%u\n", &val) != 1)
			return -EINVAL;
		for_each_possible_cpu(i)
			per_cpu(sync_info, i).input_boost_freq = val;
		for (i = 0; i < cluster_cnt; i++) {
			wake_cluster[i] = val?1:0;
		}
		goto check_enable;
	}

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > num_possible_cpus())
			return -EINVAL;

		per_cpu(sync_info, cpu).input_boost_freq = val;
		cp = strchr(cp, ' ');
		cp++;
	}

	//assign input boost freq to all cpus in the same cluster
	for (i = 0; i < cluster_cnt; i++) {
		boost_freq = 0;

		for_each_cpu(j, &cluster_cpus[i]) {
			if (!boost_freq && per_cpu(sync_info, j).input_boost_freq != 0) {
				boost_freq = per_cpu(sync_info, j).input_boost_freq;
			}
			else {
				per_cpu(sync_info, j).input_boost_freq = boost_freq;
			}
		}

		if (boost_freq)
			wake_cluster[i] = 1;
		else
			wake_cluster[i] = 0;
	}

check_enable:
	for_each_possible_cpu(i) {
		if (per_cpu(sync_info, i).input_boost_freq) {
			enabled = true;
			break;
		}
	}
	input_boost_enabled = enabled;

	return 0;
}

static int get_input_boost_freq(char *buf, const struct kernel_param *kp)
{
	int cnt = 0, cpu;
	struct cpu_sync *s;

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu, s->input_boost_freq);
	}
	cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static const struct kernel_param_ops param_ops_input_boost_freq = {
	.set = set_input_boost_freq,
	.get = get_input_boost_freq,
};
module_param_cb(input_boost_freq, &param_ops_input_boost_freq, NULL, 0644);

/*
 * The CPUFREQ_ADJUST notifier is used to override the current policy min to
 * make sure policy min >= boost_min. The cpufreq framework then does the job
 * of enforcing the new policy.
 *
 * The sync kthread needs to run on the CPU in question to avoid deadlocks in
 * the wake up code. Achieve this by binding the thread to the respective
 * CPU. But a CPU going offline unbinds threads from that CPU. So, set it up
 * again each time the CPU comes back up. We can use CPUFREQ_START to figure
 * out a CPU is coming online instead of registering for hotplug notifiers.
 */
static int boost_adjust_notify(struct notifier_block *nb, unsigned long val,
				void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	struct cpu_sync *s = &per_cpu(sync_info, cpu);
	unsigned int ib_min = s->input_boost_min;

	switch (val) {
	case CPUFREQ_ADJUST:
		if (!ib_min)
			break;

		pr_debug("CPU%u policy min before boost: %u kHz\n",
			 cpu, policy->min);
		pr_debug("CPU%u boost min: %u kHz\n", cpu, ib_min);

		cpufreq_verify_within_limits(policy, ib_min, UINT_MAX);

		pr_debug("CPU%u policy min after boost: %u kHz\n",
			 cpu, policy->min);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block boost_adjust_nb = {
	.notifier_call = boost_adjust_notify,
};

static void update_policy_online(void)
{
	unsigned int i, j;

	/* Re-evaluate policy to trigger adjust notifier for online CPUs */
	get_online_cpus();

	for (i = 0; i < cluster_cnt; i++) {
		for_each_online_cpu(j) {
			if (cpumask_test_cpu(j, &cluster_cpus[i])) {
				cpufreq_update_policy(j);
				break;
			}
		}
	}
	put_online_cpus();
}

static void do_input_boost_rem(struct work_struct *work)
{
	unsigned int i, ret;
	struct cpu_sync *i_sync_info;

	/* Reset the input_boost_min for all CPUs in the system */
	pr_debug("Resetting input boost min for all CPUs\n");
	for_each_possible_cpu(i) {
		i_sync_info = &per_cpu(sync_info, i);
		i_sync_info->input_boost_min = 0;
	}

	/* Update policies for all online CPUs */
	update_policy_online();

	if (sched_boost_active) {
		ret = sched_set_boost(0);
		if (ret)
			pr_err("cpu-boost: HMP boost disable failed\n");
		sched_boost_active = false;
	}
}

static int do_input_boost(void *data)
{
	struct cpu_sync *i_sync_info, *cpu_sync_info;
	struct cpufreq_policy policy;
	int ret, i, cpu;
	struct cpumask *mask = (struct cpumask *)data;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		if (kthread_should_stop())
			break;

		set_current_state(TASK_RUNNING);

		get_online_cpus();

		for_each_online_cpu(i) {
			if (cpumask_test_cpu(i, mask)) {
				ret = cpufreq_get_policy(&policy, i);
				if (ret)
					goto bail_incorrect_governor;

				cpu = policy.cpu;
				i_sync_info = &per_cpu(sync_info, i);
				cpu_sync_info = &per_cpu(sync_info, cpu);
				cpu_sync_info->input_boost_min = i_sync_info->input_boost_freq;

				if (policy.min < cpu_sync_info->input_boost_min)
					cpufreq_update_policy(i);

				if (cpu_sync_info->input_boost_min)
					break;
			}
		}

		/* Enable scheduler boost to migrate tasks to big cluster */
		if (sched_boost_on_input) {
			ret = sched_set_boost(1);
			if (ret)
				pr_err("cpu-boost: HMP boost enable failed\n");
			else
				sched_boost_active = true;
		}

bail_incorrect_governor:
		put_online_cpus();
	}

	return 0;
}

static void cpuboost_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;
	int need_boost = 0, i;

	if (!input_boost_enabled)
		return;

	/* touch down. */
	if (type == EV_ABS && code == ABS_MT_TRACKING_ID && value != -1)
		need_boost = 1;

	/* press key */
	if (type == EV_KEY && value == 1 &&
		(code == KEY_POWER || code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN))
		need_boost = 1;

	if (!need_boost)
		return;

	now = ktime_to_us(ktime_get());
	if (now - last_input_time < input_boost_ms * USEC_PER_MSEC) {
		return;
}

	cancel_delayed_work(&input_boost_rem);

	for (i = 0; i < cluster_cnt; i++) {
		if (wake_cluster[i])
			wake_up_process(up_task[i]);
	}

	queue_delayed_work(cpu_boost_wq, &input_boost_rem, msecs_to_jiffies(input_boost_ms));

	last_input_time = ktime_to_us(ktime_get());
}

static int cpuboost_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cpuboost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler cpuboost_input_handler = {
	.event          = cpuboost_input_event,
	.connect        = cpuboost_input_connect,
	.disconnect     = cpuboost_input_disconnect,
	.name           = "cpu-boost",
	.id_table       = cpuboost_ids,
};

static int cpu_boost_init(void)
{
	int cpu, ret;
	int i;
	struct cpu_sync *s;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };
	struct task_struct *pthread;

	cluster_cnt = get_kernel_cluster_info(cluster_id, cluster_cpus);

	if (cluster_cnt <= 0) {
		pr_err("Invalid number of cluster number : 0\n");
		return -EINVAL;
	}

	cpu_boost_wq = alloc_workqueue("cpuboost_wq", WQ_HIGHPRI, 0);
	if (!cpu_boost_wq)
		return -EFAULT;

	INIT_DELAYED_WORK(&input_boost_rem, do_input_boost_rem);

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		s->cpu = cpu;
	}

	for (i = 0; i < cluster_cnt; i++) {
		pthread = kthread_create(do_input_boost, (void*)&cluster_cpus[i], "input_boost_task%d",i);
		if (likely(!IS_ERR(pthread))) {
			sched_setscheduler_nocheck(pthread, SCHED_FIFO, &param);
			get_task_struct(pthread);
			up_task[i] = pthread;
		}
	}

	cpufreq_register_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);
	ret = input_register_handler(&cpuboost_input_handler);

	return ret;
}
late_initcall(cpu_boost_init);
