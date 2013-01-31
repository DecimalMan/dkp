/*
 *  drivers/cpufreq/cpufreq_freelunch.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *            (C)  2012 Ryan Pennucci <decimalman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* Current (Known) Issues:
 * - Interactive mode defers forever in lockscreen.  Seems unfixable without
 *   causing other issues, and I don't care anyway.  Don't leave your
 *   lockscreen on.  Problem solved.
 * - Recents and home animations are sometimes choppy.  Seems unfixable, and
 *   even Interactive has issues with them (though to a lesser extent).
 * - XDA app scrolling sucks.  Complain to Tapatalk, not me.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/slab.h>

enum interaction_flags {
	IFLAG_PRESSED = 1,
	IFLAG_RUNNING = 2,
	IFLAG_ENABLED = 1 | 2,
};
#define IGF(x) (this_dbs_info->is_interactive & IFLAG_##x)
#define ISF(x) this_dbs_info->is_interactive |= IFLAG_##x
#define IUF(x) this_dbs_info->is_interactive &= ~IFLAG_##x

/* I was going to write the code to dynamically allocate this, but your mom
 * called me back to bed.
 */
#define PREV_SAMPLES_MAX 5

// {{{1 tuner crap
static void do_dbs_timer(struct work_struct *work);

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	unsigned int requested_freq;
	int cpu;
	unsigned int enable:1;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_dbs_timer invocation. We do not want do_dbs_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;

	int hotplug_cycle;

	/* Interaction hack stuff */
	int is_interactive;
	unsigned int defer_cycles;
	unsigned int deferred_return;
	unsigned int max_freq;
	unsigned int prev_loads[PREV_SAMPLES_MAX];
	int prev_idx;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, cs_cpu_dbs_info);

static unsigned int dbs_enable;	/* number of CPUs using this policy */

/* Hotplug stuff */
static struct work_struct cpu_up_work;
static struct work_struct cpu_down_work;

/*
 * dbs_mutex protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct dbs_tuners {
	unsigned int sampling_rate;
	unsigned int ignore_nice;

	unsigned int hotplug_up_cycles;
	unsigned int hotplug_down_cycles;
	unsigned int hotplug_up_load;
	unsigned int hotplug_up_usage;
	unsigned int hotplug_down_usage;

	unsigned int overestimate_khz;

	unsigned int interaction_sampling_rate;
	unsigned int interaction_overestimate_khz;
	unsigned int interaction_return_usage;
	unsigned int interaction_return_cycles;

	unsigned int interaction_samples;
	unsigned int interaction_hispeed;
	unsigned int max_coeff;
} dbs_tuners_ins = {
	/* Pretty reasonable defaults */
	.sampling_rate = 35000, /* 2 vsyncs */
	.ignore_nice = 0,
	.hotplug_up_cycles = 3,
	.hotplug_down_cycles = 3,
	.hotplug_up_load = 3,
	.hotplug_up_usage = 40,
	.hotplug_down_usage = 15,
	.overestimate_khz = 75000,
	.interaction_sampling_rate = 10000,
	.interaction_overestimate_khz = 175000,
	.interaction_return_usage = 15,
	.interaction_return_cycles = 4, /* 3 vsyncs */
	.interaction_samples = 3, /* 2 vsyncs */
	.interaction_hispeed = 1188000,
	.max_coeff = 50,
};
// }}}
// {{{2 support function crap
static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
							cputime64_t *wall)
{
	cputime64_t idle_time;
	cputime64_t cur_wall_time;
	cputime64_t busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());
	busy_time = cputime64_add(kstat_cpu(cpu).cpustat.user,
			kstat_cpu(cpu).cpustat.system);

	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.irq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.softirq);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.steal);
	busy_time = cputime64_add(busy_time, kstat_cpu(cpu).cpustat.nice);

	idle_time = cputime64_sub(cur_wall_time, busy_time);
	if (wall)
		*wall = (cputime64_t)jiffies_to_usecs(cur_wall_time);

	return (cputime64_t)jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);

	return idle_time;
}

/* keep track of frequency transitions */
static int
dbs_cpufreq_notifier(struct notifier_block *nb, unsigned long val,
		     void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpu_dbs_info_s *this_dbs_info = &per_cpu(cs_cpu_dbs_info,
							freq->cpu);

	struct cpufreq_policy *policy;

	if (!this_dbs_info->enable)
		return 0;

	policy = this_dbs_info->cur_policy;

	/*
	 * we only care if our internally tracked freq moves outside
	 * the 'valid' ranges of freqency available to us otherwise
	 * we do not change it
	*/
	if (this_dbs_info->requested_freq > policy->max
			|| this_dbs_info->requested_freq < policy->min)
		this_dbs_info->requested_freq = freq->new;

	return 0;
}

static struct notifier_block dbs_cpufreq_notifier_block = {
	.notifier_call = dbs_cpufreq_notifier
};
// }}}
// {{{2 sysfs crap
/************************** sysfs interface ************************/
static ssize_t show_sampling_rate_min(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", 10000);
}

define_one_global_ro(sampling_rate_min);

/* cpufreq_freelunch Governor Tunables */
#define show_one(file_name, object)							\
static ssize_t show_##file_name								\
(struct kobject *kobj, struct attribute *attr, char *buf)	\
{															\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
#define i_am_lazy(f, min, max)						\
show_one(f,f)										\
static ssize_t store_##f							\
(struct kobject *a, struct attribute *b,			\
				   const char *buf, size_t count)	\
{													\
	unsigned int input; int ret;					\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1 || input < min || input > max) return -EINVAL;	\
	dbs_tuners_ins.f = input;						\
	return count;									\
}													\
define_one_global_rw(f);

show_one(sampling_rate, sampling_rate);
show_one(ignore_nice_load, ignore_nice);
i_am_lazy(hotplug_up_cycles, 0, 10)
i_am_lazy(hotplug_down_cycles, 0, 10)
i_am_lazy(hotplug_up_load, 0, 10)
i_am_lazy(hotplug_up_usage, 0, 100)
i_am_lazy(hotplug_down_usage, 0, 100)
i_am_lazy(overestimate_khz, 0, 350000)
i_am_lazy(interaction_sampling_rate, 10000, 1000000)
i_am_lazy(interaction_overestimate_khz, 0, 350000)
i_am_lazy(interaction_return_usage, 0, 100)
i_am_lazy(interaction_return_cycles, 0, 100)
i_am_lazy(interaction_samples, 1, PREV_SAMPLES_MAX)
i_am_lazy(interaction_hispeed, 0, 4000000)
i_am_lazy(max_coeff, 1, 1000)

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	dbs_tuners_ins.sampling_rate = max(input, (unsigned int)10000);
	return count;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == dbs_tuners_ins.ignore_nice) /* nothing to do */
		return count;

	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(cs_cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&dbs_info->prev_cpu_wall);
		if (dbs_tuners_ins.ignore_nice)
			dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;
	}
	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(ignore_nice_load);

static struct attribute *dbs_attributes[] = {
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&ignore_nice_load.attr,
	&hotplug_up_cycles.attr,
	&hotplug_down_cycles.attr,
	&hotplug_up_load.attr,
	&hotplug_up_usage.attr,
	&hotplug_down_usage.attr,
	&overestimate_khz.attr,
	&interaction_sampling_rate.attr,
	&interaction_overestimate_khz.attr,
	&interaction_return_usage.attr,
	&interaction_return_cycles.attr,
	&interaction_samples.attr,
	&interaction_hispeed.attr,
	&max_coeff.attr,
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "freelunch",
};

/************************** sysfs end ************************/
// }}}
// {{{1 useful crap
static void do_cpu_up(struct work_struct *work) {
	if (num_online_cpus() == 1) cpu_up(1);
}
static void do_cpu_down(struct work_struct *work) {
	if (num_online_cpus() > 1) cpu_down(1);
}

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	unsigned int load;
	cputime64_t cur_wall_time, cur_idle_time;
	unsigned int idle_time, wall_time;

	struct cpufreq_policy *policy;

	unsigned int min_freq, overestimate, hispeed;

	/* Calculate load for this processor only.  The assumption is that we're
	 * running on an aSMP processor where each core has its own instance.
	 *
	 * XXX This will not work on processors with linked frequencies, since they
	 * have multiple cores per policy.
	 */
	policy = this_dbs_info->cur_policy;

	cur_idle_time = get_cpu_idle_time(policy->cpu, &cur_wall_time);

	wall_time = (unsigned int) cputime64_sub(cur_wall_time,
		this_dbs_info->prev_cpu_wall);
	this_dbs_info->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int) cputime64_sub(cur_idle_time,
		this_dbs_info->prev_cpu_idle);
	this_dbs_info->prev_cpu_idle = cur_idle_time;

	if (dbs_tuners_ins.ignore_nice) {
		cputime64_t cur_nice;
		unsigned long cur_nice_jiffies;

		cur_nice = cputime64_sub(kstat_cpu(policy->cpu).cpustat.nice,
			this_dbs_info->prev_cpu_nice);
		cur_nice_jiffies = (unsigned long)
			cputime64_to_jiffies64(cur_nice);

		this_dbs_info->prev_cpu_nice = kstat_cpu(policy->cpu).cpustat.nice;
		idle_time += jiffies_to_usecs(cur_nice_jiffies);
	}

	/* Apparently, this happens. */
	if (idle_time > wall_time) return;

	/* Not needed, but why not? */
	if (wall_time > 1000) {
		wall_time /= 10;
		idle_time /= 10;
	}

	/* Assume we've only run for a small fraction of a second. */
	load = policy->cur / wall_time * (wall_time - idle_time);

	/* Hotplug?
	 * This used to happen after freq setting, but it's handy to be able to
	 * return where appropriate.
	 */
	if (num_online_cpus() == 1) {
#if 1
		if (nr_running() >= dbs_tuners_ins.hotplug_up_load) {
			if ((this_dbs_info->hotplug_cycle++ >= dbs_tuners_ins.hotplug_up_cycles) &&
				load > policy->max * dbs_tuners_ins.hotplug_up_usage / 100) {
#else
		if (nr_running() >= dbs_tuners_ins.hotplug_up_load &&
			load > policy->max * dbs_tuners_ins.hotplug_up_usage / 100) {
			if (this_dbs_info->hotplug_cycle++ >= dbs_tuners_ins.hotplug_up_cycles) {
#endif
				schedule_work_on(0, &cpu_up_work);
				this_dbs_info->hotplug_cycle = 0;
			}
		} else this_dbs_info->hotplug_cycle = 0;
	} else {
		if (load < policy->max * dbs_tuners_ins.hotplug_down_usage / 100) {
			if (this_dbs_info->hotplug_cycle++ >= dbs_tuners_ins.hotplug_down_cycles) {
				schedule_work_on(0, &cpu_down_work);
				this_dbs_info->hotplug_cycle = 0;
			}
		} else this_dbs_info->hotplug_cycle = 0;
	}

	if (IGF(ENABLED)) {
		if (!IGF(PRESSED)) {
			this_dbs_info->deferred_return++;
			if (load < policy->max * dbs_tuners_ins.interaction_return_usage / 100) {
				if (this_dbs_info->defer_cycles++ >= dbs_tuners_ins.interaction_return_cycles) {
					IUF(ENABLED);
					printk(KERN_DEBUG "freelunch: deferred noninteractive %u cycles.\n",
						this_dbs_info->deferred_return);
				}
			} else this_dbs_info->defer_cycles = 0;
		}
		overestimate = dbs_tuners_ins.interaction_overestimate_khz;
		hispeed = dbs_tuners_ins.interaction_hispeed;
	} else {
		overestimate = dbs_tuners_ins.overestimate_khz;
		hispeed = 0;
	}

	/* Update max_freq & min_freq:
	 * Both will always be >= load
	 */
	if (load > policy->cur - overestimate) {
		this_dbs_info->max_freq = max(load, this_dbs_info->max_freq);
	} else {
		unsigned int fml = overestimate * dbs_tuners_ins.max_coeff / 100;
		if (this_dbs_info->max_freq < hispeed && load < policy->min)
			this_dbs_info->max_freq = min(hispeed, this_dbs_info->max_freq + fml);
		else
			this_dbs_info->max_freq -= fml;
		if (this_dbs_info->max_freq < load)
			this_dbs_info->max_freq = load;
	}

	if (IGF(ENABLED)) {
		int idx, cnt;
		min_freq = 0;
		this_dbs_info->prev_loads[this_dbs_info->prev_idx % dbs_tuners_ins.interaction_samples] = load;
		this_dbs_info->prev_idx++;
		for (idx = min((int)dbs_tuners_ins.interaction_samples, this_dbs_info->prev_idx),
			cnt = 0; idx >= 0; idx--) {
			if (this_dbs_info->prev_loads[idx] >= load) {
				min_freq += this_dbs_info->prev_loads[idx];
				cnt++;
			}
		}
		if (!cnt) {
			printk(KERN_DEBUG "freelunch: something is seriously wrong\n");
			return;
		}
		min_freq /= cnt;
	} else
		min_freq = load;

	/* Set frequency */
	if (load > policy->cur - overestimate) {
		unsigned int dist = 1000 * (load + overestimate - policy->cur) / overestimate;
		this_dbs_info->requested_freq = (dist * this_dbs_info->max_freq +
			(1000 - dist) * min_freq) / 1000 + overestimate;
	} else {
		this_dbs_info->requested_freq = min_freq + overestimate;
	}

	__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
		CPUFREQ_RELATION_H);
}
// }}}
// {{{2 cpufreq crap
static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *this_dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = this_dbs_info->cpu;

	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay;
	if (IGF(ENABLED))
		delay = usecs_to_jiffies(dbs_tuners_ins.interaction_sampling_rate);
	else
		delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);

	delay -= jiffies % delay;

	mutex_lock(&this_dbs_info->timer_mutex);

	dbs_check_cpu(this_dbs_info);

	schedule_delayed_work_on(cpu, &this_dbs_info->work, delay);
	mutex_unlock(&this_dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	delay -= jiffies % delay;

	dbs_info->enable = 1;
	INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
	schedule_delayed_work_on(dbs_info->cpu, &dbs_info->work, delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	dbs_info->enable = 0;
	cancel_delayed_work_sync(&dbs_info->work);
}

static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int j;
	int rc;

	this_dbs_info = &per_cpu(cs_cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		mutex_lock(&dbs_mutex);

		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(cs_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall);
			if (dbs_tuners_ins.ignore_nice) {
				j_dbs_info->prev_cpu_nice =
						kstat_cpu(j).cpustat.nice;
			}
		}
		this_dbs_info->requested_freq = policy->cur;
		this_dbs_info->hotplug_cycle = 0;
		/* Dirty hack */
		if (cpu > 0)
			this_dbs_info->is_interactive = per_cpu(cs_cpu_dbs_info, 0).is_interactive;

		mutex_init(&this_dbs_info->timer_mutex);
		dbs_enable++;
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			rc = sysfs_create_group(cpufreq_global_kobject,
						&dbs_attr_group);
			if (rc) {
				mutex_unlock(&dbs_mutex);
				return rc;
			}

			cpufreq_register_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
		}
		mutex_unlock(&dbs_mutex);

		dbs_timer_init(this_dbs_info);

		break;

	case CPUFREQ_GOV_STOP:
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);
		dbs_enable--;
		mutex_destroy(&this_dbs_info->timer_mutex);

		/*
		 * Stop the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 0)
			cpufreq_unregister_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);

		mutex_unlock(&dbs_mutex);
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);

		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(
					this_dbs_info->cur_policy,
					policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(
					this_dbs_info->cur_policy,
					policy->min, CPUFREQ_RELATION_L);
		mutex_unlock(&this_dbs_info->timer_mutex);

		break;

	case CPUFREQ_GOV_INTERACT:
		mutex_lock(&this_dbs_info->timer_mutex);
		this_dbs_info->max_freq = max(this_dbs_info->max_freq,
			dbs_tuners_ins.interaction_hispeed);
		if (!IGF(ENABLED)) {
			int idx;

			this_dbs_info->prev_idx = 0;
			for (idx = 0; idx < PREV_SAMPLES_MAX; idx++)
				this_dbs_info->prev_loads[idx] = 0;
			ISF(ENABLED);

			if (cancel_delayed_work_sync(&this_dbs_info->work)) {
				this_dbs_info->prev_cpu_idle =
					get_cpu_idle_time(policy->cpu, &this_dbs_info->prev_cpu_wall);
				schedule_delayed_work_on(this_dbs_info->cpu, &this_dbs_info->work,
					usecs_to_jiffies(dbs_tuners_ins.interaction_sampling_rate));
			}
		}
		mutex_unlock(&this_dbs_info->timer_mutex);

		break;

	case CPUFREQ_GOV_NOINTERACT:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (IGF(PRESSED)) {
			IUF(PRESSED);
			this_dbs_info->defer_cycles = 0;
			this_dbs_info->deferred_return = 0;
		}
		mutex_unlock(&this_dbs_info->timer_mutex);

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_FREELUNCH
static
#endif
struct cpufreq_governor cpufreq_gov_freelunch = {
	.name			= "freelunch",
	.governor		= cpufreq_governor_dbs,
	.max_transition_latency	= 10000000,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	INIT_WORK(&cpu_up_work, do_cpu_up);
	INIT_WORK(&cpu_down_work, do_cpu_down);
	return cpufreq_register_governor(&cpufreq_gov_freelunch);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_freelunch);
}

MODULE_AUTHOR("Ryan Pennucci <decimalman@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_freelunch' -- an incredibly simple hotplugging governor.");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_FREELUNCH
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
// }}}
// vim:ts=4:sw=4:fdm=marker:fdl=1
