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
MODULE_DESCRIPTION("Overrides latency in cpuidle");

static unsigned int exit_latency[CPUIDLE_STATE_MAX];
static unsigned int target_residency[CPUIDLE_STATE_MAX];

static void backup(void)
{
	int i;
	struct cpuidle_driver *driver;

	driver = cpuidle_get_driver();
	for (i = 0; i < driver->state_count; i++) {
		exit_latency[i] = driver->states[i].exit_latency;
		target_residency[i] = driver->states[i].target_residency;
		driver->states[i].exit_latency = 0;
		driver->states[i].target_residency = 0;
	}
}

static void restore(void)
{
	int i;
	struct cpuidle_driver *driver;

	driver = cpuidle_get_driver();
	for (i = 0; i < driver->state_count; i++) {
		//printk(KERN_INFO "exlat: %d %u %u\n", i, info->exit_latency[i], info->target_residency[i]);
		driver->states[i].exit_latency = exit_latency[i];
		driver->states[i].target_residency = target_residency[i];
	}
	/*
	driver->states[0].exit_latency = 0;
	driver->states[0].target_residency = 0;

	driver->states[1].exit_latency = 2;
	driver->states[1].target_residency = 2;

	driver->states[2].exit_latency = 10;
	driver->states[2].target_residency = 20;

	driver->states[3].exit_latency = 70;
	driver->states[3].target_residency = 100;

	driver->states[4].exit_latency = 85;
	driver->states[4].target_residency = 200;

	driver->states[5].exit_latency = 124;
	driver->states[5].target_residency = 800;

	driver->states[5].exit_latency = 200;
	driver->states[5].target_residency = 800;
	*/
}

static int __init exlat_init(void)
{
	int i;
	struct cpuidle_driver *driver;

	driver = cpuidle_get_driver();
	printk(KERN_INFO "exlat: %s\n", driver->name);
	for (i = 0; i < driver->state_count; i++) {
		printk(KERN_INFO "exlat: %s\t%u\t%u\n", 
				driver->states[i].name, 
				driver->states[i].exit_latency,
				driver->states[i].target_residency
			);
	}

	backup();

	printk(KERN_INFO "exlat: inserted\n");
	return 0;
}

static void __exit exlat_cleanup(void)
{
	restore();

	printk(KERN_INFO "exlat: removed\n");
}

module_init(exlat_init);
module_exit(exlat_cleanup);
