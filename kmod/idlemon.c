#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/cpuidle.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eric Clark");
MODULE_DESCRIPTION("Constructs a distribution of idle times");

#define NBUCKETS 15

struct idlemon_data {
	ktime_t time_enter;
	ktime_t time_exit;
	unsigned int exit_latency;
	ktime_t diff_active;
	ktime_t diff_idle;
	s64 active[NBUCKETS];
	s64 idle[NBUCKETS];
	ktime_t activesum[NBUCKETS];
	ktime_t idlesum[NBUCKETS];
};

DEFINE_PER_CPU(struct idlemon_data, mondata);
static DEFINE_SPINLOCK(summary_lock);
static struct idlemon_data summary;

#ifndef BUCK1
static inline int which_bucket(ktime_t kdiff) {
	s64 diff = ktime_to_us(kdiff);
	if (diff < -1000) {
		return 0;
	} else if (diff < -100) {
		return 1;
	} else if (diff < -10) { 
		return 2;
	} else if (diff < 0) {
		return 3;
	} else if (diff <= 2) {
		return 4;
	} else if (diff <= 10) {
		return 5;
	} else if (diff <= 20) {
		return 6;
	} else if (diff <= 70) {
		return 7; 
	} else if (diff <= 85) {
		return 8;
	} else if (diff <= 100) {
		return 9;
	} else if (diff <= 124) {
		return 10;
	} else if (diff <= 200) {
		return 11;
	} else if (diff <= 800) {
		return 12;
	} else if (diff <= 1000000) {
		return 13;
	} else {
		return 14;
	}
}
#else
static inline int which_bucket(ktime_t kdiff) {
	s64 diff = ktime_to_us(kdiff);
	if (diff <= 0) {
		return 0;
	} else if (diff <= 1) {
		return 1;
	} else if (diff <= 2) { 
		return 2;
	} else if (diff <= 4) {
		return 3;
	} else if (diff <= 8) {
		return 4;
	} else if (diff <= 16) {
		return 5;
	} else if (diff <= 32) {
		return 6;
	} else if (diff <= 64) {
		return 7; 
	} else if (diff <= 128) {
		return 8;
	} else if (diff <= 256) {
		return 9;
	} else if (diff <= 512) {
		return 10;
	} else if (diff <= 1024) {
		return 11;
	} else if (diff <= 2048) {
		return 12;
	} else if (diff <= 4096) {
		return 13;
	} else {
		return 14;
	}
}
#endif

static inline s64 fixup(s64 exit_latency, s64 measured_us) {
	if (measured_us > 2 * exit_latency) {
		measured_us -= exit_latency;
	} else {
		measured_us /= 2;
	}

	return measured_us;
}

static void jsched_idle_set_state(struct cpuidle_state *idle_state)
{
	int bin;
	struct idlemon_data *data = get_cpu_ptr(&mondata);

	if (idle_state != NULL) {
		data->time_enter = ns_to_ktime(local_clock());
		data->diff_active = ktime_sub(data->time_enter, data->time_exit);

		bin = which_bucket(data->diff_active);
		data->active[bin]++;
		data->activesum[bin] = ktime_add(data->activesum[bin], data->diff_active);
		data->exit_latency = idle_state->exit_latency;
	} else {
		data->time_exit = ns_to_ktime(local_clock());
		data->diff_idle = ktime_sub(data->time_exit, data->time_enter);

		bin = which_bucket(data->diff_idle);

		data->idle[bin]++;
		data->idlesum[bin] = ktime_add(data->idlesum[bin], data->diff_idle);
	}

	put_cpu_ptr(&data);
	jprobe_return();
}

// collect and reset subroutines

static void reset_percpu(void *unused)
{
	int i;
	struct idlemon_data *data = get_cpu_ptr(&mondata);

	for (i = 0; i < NBUCKETS; i++) {
		data->active[i] = 0;
		data->idle[i] = 0;
		data->activesum[i] = ns_to_ktime(0);
		data->idlesum[i] = ns_to_ktime(0);
		//data->idle_nowake[i] = 0;
	}

	put_cpu_ptr(&data);
}

static void reset_summary(void)
{
	int i;

	spin_lock(&summary_lock);
	for (i = 0; i < NBUCKETS; i++) {
		summary.active[i] = 0;
		summary.idle[i] = 0;
		summary.activesum[i] = ns_to_ktime(0);
		summary.idlesum[i] = ns_to_ktime(0);
	}
	spin_unlock(&summary_lock);
}

static void reset_all(void)
{
	reset_summary();

	on_each_cpu(reset_percpu, NULL, 1);
}

static void collect_percpu(void *unused)
{
	int i;
	struct idlemon_data *data = get_cpu_ptr(&mondata);

	spin_lock(&summary_lock);
	for (i = 0; i < NBUCKETS; i++) {
		summary.active[i] += data->active[i];
		summary.idle[i] += data->idle[i];
		summary.activesum[i] = ktime_add(summary.activesum[i], data->activesum[i]);
		summary.idlesum[i] = ktime_add(summary.idlesum[i], data->idlesum[i]);

		data->active[i] = 0;
		data->idle[i] = 0;
		data->activesum[i] = ns_to_ktime(0);
		data->idlesum[i] = ns_to_ktime(0);
	}
	spin_unlock(&summary_lock);

	put_cpu_ptr(&data);
}

static void collect_summary(void)
{
	on_each_cpu(collect_percpu, NULL, 1);
}

// sequence iterator subroutines
static void *idlemon_seq_start(struct seq_file *s, loff_t *pos)
{
	loff_t *spos = kmalloc(sizeof(loff_t), GFP_KERNEL);
	if (!spos || *pos >= NBUCKETS)
		return NULL;

	*spos = *pos;
	
	return spos;
}

static void *idlemon_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	loff_t *spos = v;
	*pos = ++*spos;

	if (*spos >= NBUCKETS)
		return NULL;

	return spos;
}

static void idlemon_seq_stop(struct seq_file *s, void *v)
{
	kfree(v);
}

static int idlemon_seq_show(struct seq_file *s, void *v)
{
	loff_t *spos = v;
	int bin = *spos;

	seq_printf(s, "%d\t%lld\t%lld\t%lld\t%lld\n", bin, summary.active[bin], summary.idle[bin], ktime_to_ns(summary.activesum[bin]), ktime_to_ns(summary.idlesum[bin]));

	return 0;
}

// set up seq_ops
static const struct seq_operations idlemon_seq_ops = {
	.start = idlemon_seq_start,
	.next = idlemon_seq_next,
	.stop = idlemon_seq_stop,
	.show = idlemon_seq_show,
};

// file_ops subroutines
static int idlemon_open(struct inode *inode, struct file *file)
{
	collect_summary();

	return seq_open(file, &idlemon_seq_ops);
}

static ssize_t idlemon_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	reset_all();

	return len;
}

// set up file_ops
static const struct file_operations idlemon_file_ops = {
	.owner = THIS_MODULE,
	.open = idlemon_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write = idlemon_write,
};

// set up probes
static struct jprobe mon_jprobe = {
	.entry = jsched_idle_set_state,
	.kp = {
		.symbol_name = "sched_idle_set_state",
	},
};

static int __init idlemon_init(void)
{
	int ret;

	reset_all();

	ret = register_jprobe(&mon_jprobe);
	if (ret < 0) {
		printk(KERN_INFO "idlemon: register_jprobe failed, returned %d\n", ret);
		return -1;
	}
	printk(KERN_INFO "idlemon: Planted jprobe at %p, handler addr %p\n",
			mon_jprobe.kp.addr, mon_jprobe.entry);

	proc_create("idlemon", 0, NULL, &idlemon_file_ops);

	printk(KERN_INFO "idlemon: inserted\n");
	return 0;
}

static void __exit idlemon_cleanup(void)
{
	unregister_jprobe(&mon_jprobe);

	remove_proc_entry("idlemon", NULL);

	printk(KERN_INFO "idlemon: removed\n");
}

module_init(idlemon_init);
module_exit(idlemon_cleanup);
