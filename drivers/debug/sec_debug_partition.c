/*
 *  drivers/debug/sec_debug_partition.c
 *
 * COPYRIGHT(C) 2006-2017 Samsung Electronics Co., Ltd. All Right Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define pr_fmt(fmt)     KBUILD_MODNAME ":%s: " fmt, __func__

#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/syscalls.h>
#include <linux/delay.h>

#include <linux/sec_bsp.h>
#include <linux/sec_debug.h>
#include <linux/sec_debug_user_reset.h>
#include <linux/sec_debug_partition.h>

#define PRINT_MSG_CYCLE	20

/* single global instance */
struct debug_partition_data_s sched_debug_data;

static int in_panic;
static int driver_initialized;
static int ap_health_initialized;
static struct delayed_work dbg_partition_notify_work;
static struct workqueue_struct *dbg_part_wq;
static struct delayed_work ap_health_work;
static ap_health_t ap_health_data;

static DEFINE_MUTEX(ap_health_work_lock);
static DEFINE_MUTEX(debug_partition_mutex);
static BLOCKING_NOTIFIER_HEAD(dbg_partition_notifier_list);

static void debug_partition_operation(struct work_struct *work)
{
	int ret;
	struct file *filp;
	mm_segment_t fs;
	struct debug_partition_data_s *sched_data =
		container_of(work, struct debug_partition_data_s,
			     debug_partition_work);
	int flag = (sched_data->direction == PARTITION_WR) ?
		(O_RDWR | O_SYNC) : O_RDONLY;
	static unsigned int err_cnt;

	if (!sched_data->value) {
		pr_err("%p %x %d %d - value is NULL!!\n",
			sched_data->value, sched_data->offset,
			sched_data->size, sched_data->direction);
		sched_data->error = -ENODATA;
		goto sched_data_err;
	}

	fs = get_fs();
	set_fs(get_ds());

	sched_data->error = 0;

	filp = filp_open(DEBUG_PARTITION_NAME, flag, 0);
	if (IS_ERR(filp)) {
		if (!(++err_cnt % PRINT_MSG_CYCLE))
			pr_err("filp_open failed: %ld[%u]\n",
			       PTR_ERR(filp), err_cnt);
		sched_data->error = PTR_ERR(filp);
		goto filp_err;
	}

	ret = vfs_llseek(filp, sched_data->offset, SEEK_SET);
	if (ret < 0) {
		pr_err("FAIL LLSEEK\n");
		sched_data->error = ret;
		goto llseek_err;
	}

	if (sched_data->direction == PARTITION_RD)
		vfs_read(filp, (char __user *)sched_data->value,
			 sched_data->size, &filp->f_pos);
	else if (sched_data->direction == PARTITION_WR)
		vfs_write(filp, (char __user *)sched_data->value,
			  sched_data->size, &filp->f_pos);

llseek_err:
	filp_close(filp, NULL);
filp_err:
	set_fs(fs);
sched_data_err:
	complete(&sched_data->work);
}

static void ap_health_work_write_fn(struct work_struct *work)
{
	int ret;
	struct file *filp;
	mm_segment_t fs;
	unsigned long delay = 5 * HZ;
	static unsigned int err_cnt;

	pr_info("start.\n");

	if (!mutex_trylock(&ap_health_work_lock)) {
		pr_err("already locked.\n");
		delay = 2 * HZ;
		goto occupied_retry;
	}

	fs = get_fs();
	set_fs(get_ds());

	filp = filp_open(DEBUG_PARTITION_NAME, (O_RDWR | O_SYNC), 0);
	if (IS_ERR(filp)) {
		if (!(++err_cnt % PRINT_MSG_CYCLE))
			pr_err("filp_open failed: %ld[%u]\n",
			       PTR_ERR(filp), err_cnt);
		goto openfail_retry;
	}

	ret = vfs_llseek(filp, SEC_DEBUG_AP_HEALTH_OFFSET, SEEK_SET);
	if (ret < 0) {
		pr_err("FAIL LLSEEK\n");
		ret = false;
		goto seekfail_retry;
	}

	vfs_write(filp, (char __user *)&ap_health_data,
		  sizeof(ap_health_t), &filp->f_pos);

	if (--ap_health_data.header.need_write)
		goto remained;

	filp_close(filp, NULL);
	set_fs(fs);

	mutex_unlock(&ap_health_work_lock);
	pr_info("end.\n");
	return;

remained:
seekfail_retry:
	filp_close(filp, NULL);
openfail_retry:
	set_fs(fs);
	mutex_unlock(&ap_health_work_lock);
occupied_retry:
	queue_delayed_work(dbg_part_wq, &ap_health_work, delay);
	pr_info("end, will retry, wr(%u).\n",
		ap_health_data.header.need_write);
}

static void init_ap_health_data(void)
{
	pr_info("start\n");

	memset((void *)&ap_health_data, 0, sizeof(ap_health_t));

	ap_health_data.header.magic = AP_HEALTH_MAGIC;
	ap_health_data.header.version = AP_HEALTH_VER;
	ap_health_data.header.size = sizeof(ap_health_t);
	ap_health_data.spare_magic1 = AP_HEALTH_MAGIC;
	ap_health_data.spare_magic2 = AP_HEALTH_MAGIC;
	ap_health_data.spare_magic3 = AP_HEALTH_MAGIC;

	while (1) {
		mutex_lock(&debug_partition_mutex);

		sched_debug_data.value = &ap_health_data;
		sched_debug_data.offset = SEC_DEBUG_AP_HEALTH_OFFSET;
		sched_debug_data.size = sizeof(ap_health_t);
		sched_debug_data.direction = PARTITION_WR;

		schedule_work(&sched_debug_data.debug_partition_work);
		wait_for_completion(&sched_debug_data.work);

		mutex_unlock(&debug_partition_mutex);
		if (!sched_debug_data.error)
			break;

		msleep(1000);
	}

	pr_info("end\n");
}

static void init_debug_partition(void)
{
	struct debug_reset_header init_reset_header;

	pr_info("start\n");

	/*++ add here need init data ++*/
	init_ap_health_data();
	/*-- add here need init data --*/

	while (1) {
		mutex_lock(&debug_partition_mutex);

		memset(&init_reset_header, 0,
		       sizeof(struct debug_reset_header));
		init_reset_header.magic = DEBUG_PARTITION_MAGIC;

		sched_debug_data.value = &init_reset_header;
		sched_debug_data.offset = SEC_DEBUG_RESET_HEADER_OFFSET;
		sched_debug_data.size = sizeof(struct debug_reset_header);
		sched_debug_data.direction = PARTITION_WR;

		schedule_work(&sched_debug_data.debug_partition_work);
		wait_for_completion(&sched_debug_data.work);

		mutex_unlock(&debug_partition_mutex);

		if (!sched_debug_data.error)
			break;

		msleep(1000);
	}

	pr_info("end\n");
}

static void check_magic_data(void)
{
	static int checked_magic;
	struct debug_reset_header partition_header = {0,};

	if (checked_magic)
		return;

	pr_info("start\n");

	while (1) {
		mutex_lock(&debug_partition_mutex);

		sched_debug_data.value = &partition_header;
		sched_debug_data.offset = SEC_DEBUG_RESET_HEADER_OFFSET;
		sched_debug_data.size = sizeof(struct debug_reset_header);
		sched_debug_data.direction = PARTITION_RD;

		schedule_work(&sched_debug_data.debug_partition_work);
		wait_for_completion(&sched_debug_data.work);

		mutex_unlock(&debug_partition_mutex);

		if (!sched_debug_data.error)
			break;

		msleep(1000);
	}

	if (partition_header.magic != DEBUG_PARTITION_MAGIC)
		init_debug_partition();

	checked_magic = 1;

	pr_info("end\n");
}

#define READ_DEBUG_PARTITION(_value, _offset, _size)		\
{								\
	mutex_lock(&debug_partition_mutex);			\
	sched_debug_data.value = _value;			\
	sched_debug_data.offset = _offset;			\
	sched_debug_data.size = _size;				\
	sched_debug_data.direction = PARTITION_RD;		\
	schedule_work(&sched_debug_data.debug_partition_work);	\
	wait_for_completion(&sched_debug_data.work);		\
	mutex_unlock(&debug_partition_mutex);			\
}

bool read_debug_partition(enum debug_partition_index index, void *value)
{
	check_magic_data();

	switch (index) {
	case debug_index_reset_ex_info:
		READ_DEBUG_PARTITION(value,
				     SEC_DEBUG_EXTRA_INFO_OFFSET,
				     SEC_DEBUG_EX_INFO_SIZE);
		break;
	case debug_index_reset_klog_info:
	case debug_index_reset_summary_info:
		READ_DEBUG_PARTITION(value,
				     SEC_DEBUG_RESET_HEADER_OFFSET,
				     sizeof(struct debug_reset_header));
		break;
	case debug_index_reset_summary:
		READ_DEBUG_PARTITION(value,
				     SEC_DEBUG_RESET_SUMMARY_OFFSET,
				     SEC_DEBUG_RESET_SUMMARY_SIZE);
		break;
	case debug_index_reset_klog:
		READ_DEBUG_PARTITION(value,
				     SEC_DEBUG_RESET_KLOG_OFFSET,
				     SEC_DEBUG_RESET_KLOG_SIZE);
		break;
	case debug_index_reset_tzlog:
		READ_DEBUG_PARTITION(value,
					SEC_DEBUG_RESET_TZLOG_OFFSET,
					SEC_DEBUG_RESET_TZLOG_SIZE);
		break;
	case debug_index_ap_health:
		READ_DEBUG_PARTITION(value,
				     SEC_DEBUG_AP_HEALTH_OFFSET,
				     SEC_DEBUG_AP_HEALTH_SIZE);
		break;
	case debug_index_reset_extrc_info:
		READ_DEBUG_PARTITION(value,
				SEC_DEBUG_RESET_EXTRC_OFFSET,
				SEC_DEBUG_RESET_EXTRC_SIZE);
		break;
	default:
		return false;
	}

	return true;
}
EXPORT_SYMBOL(read_debug_partition);

bool write_debug_partition(enum debug_partition_index index, void *value)
{
	check_magic_data();

	switch (index) {
	case debug_index_reset_klog_info:
	case debug_index_reset_summary_info:
		mutex_lock(&debug_partition_mutex);
		sched_debug_data.value =
			(struct debug_reset_header *)value;
		sched_debug_data.offset = SEC_DEBUG_RESET_HEADER_OFFSET;
		sched_debug_data.size =
			sizeof(struct debug_reset_header);
		sched_debug_data.direction = PARTITION_WR;
		schedule_work(&sched_debug_data.debug_partition_work);
		wait_for_completion(&sched_debug_data.work);
		mutex_unlock(&debug_partition_mutex);
		break;
	case debug_index_reset_ex_info:
	case debug_index_reset_summary:
		// do nothing.
		break;
	default:
		return false;
	}

	return true;
}
EXPORT_SYMBOL(write_debug_partition);

ap_health_t *ap_health_data_read(void)
{
	if (!driver_initialized)
		return NULL;

	if (ap_health_initialized)
		goto out;

	if (in_interrupt()) {
		pr_info("skip read opt.\n");
		return NULL;
	}

	read_debug_partition(debug_index_ap_health, (void *)&ap_health_data);

	if (ap_health_data.header.magic != AP_HEALTH_MAGIC ||
		ap_health_data.header.version != AP_HEALTH_VER ||
		ap_health_data.header.size != sizeof(ap_health_t) ||
		ap_health_data.spare_magic1 != AP_HEALTH_MAGIC ||
		ap_health_data.spare_magic2 != AP_HEALTH_MAGIC ||
		ap_health_data.spare_magic3 != AP_HEALTH_MAGIC ||
		is_boot_recovery()) {
		init_ap_health_data();
	}

	ap_health_initialized = 1;
out:
	return &ap_health_data;
}
EXPORT_SYMBOL(ap_health_data_read);

int ap_health_data_write(ap_health_t *data)
{
	if (!driver_initialized || !data || !ap_health_initialized)
		return -ENODATA;

	data->header.need_write++;

	if (!in_panic)
		queue_delayed_work(dbg_part_wq, &ap_health_work, 0);

	return 0;
}
EXPORT_SYMBOL(ap_health_data_write);

int dbg_partition_notifier_register(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(
			&dbg_partition_notifier_list, nb);
}
EXPORT_SYMBOL(dbg_partition_notifier_register);

static void debug_partition_do_notify(struct work_struct *work)
{
	blocking_notifier_call_chain(&dbg_partition_notifier_list,
				     DBG_PART_DRV_INIT_DONE, NULL);
}

static int dbg_partitioni_panic_prepare(struct notifier_block *nb,
		unsigned long event, void *data)
{
	in_panic = 1;
	return NOTIFY_DONE;
}

static struct notifier_block dbg_partition_panic_notifier_block = {
	.notifier_call = dbg_partitioni_panic_prepare,
};

static int __init sec_debug_partition_init(void)
{
	pr_info("start\n");

	sched_debug_data.offset = 0;
	sched_debug_data.direction = 0;
	sched_debug_data.size = 0;
	sched_debug_data.value = NULL;

	init_completion(&sched_debug_data.work);
	INIT_WORK(&sched_debug_data.debug_partition_work,
			debug_partition_operation);
	INIT_DELAYED_WORK(&dbg_partition_notify_work,
			debug_partition_do_notify);
	INIT_DELAYED_WORK(&ap_health_work, ap_health_work_write_fn);

	dbg_part_wq = create_singlethread_workqueue("glink_lbsrv");
	if (!dbg_part_wq) {
		pr_err("fail to create dbg_part_wq!\n");
		return -EFAULT;
	}
	atomic_notifier_chain_register(&panic_notifier_list,
				       &dbg_partition_panic_notifier_block);

	driver_initialized = DRV_INITIALIZED;
	schedule_delayed_work(&dbg_partition_notify_work, 2 * HZ);
	pr_info("end\n");

	return 0;
}

static void __exit sec_debug_partition_exit(void)
{
	driver_initialized = DRV_UNINITIALIZED;
	cancel_work_sync(&sched_debug_data.debug_partition_work);
	cancel_delayed_work_sync(&dbg_partition_notify_work);
	pr_info("exit\n");
}

module_init(sec_debug_partition_init);
module_exit(sec_debug_partition_exit);
