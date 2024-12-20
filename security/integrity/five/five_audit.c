/*
 * Audit calls for FIVE audit subsystem.
 *
 * Copyright (C) 2017 Samsung Electronics, Inc.
 * Egor Uleyskiy, <e.uleyskiy@samsung.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/audit.h>
#include <linux/task_integrity.h>
#include "five.h"
#include "five_audit.h"

static void five_audit_msg(struct task_struct *task, struct file *file,
		const char *op, enum task_integrity_value prev,
		enum task_integrity_value tint, const char *cause, int result);

#ifdef CONFIG_FIVE_AUDIT_VERBOSE
void five_audit_verbose(struct task_struct *task, struct file *file,
		const char *op, enum task_integrity_value prev,
		enum task_integrity_value tint, const char *cause, int result)
{
	five_audit_msg(task, file, op, prev, tint, cause, result);
}
#else
void five_audit_verbose(struct task_struct *task, struct file *file,
		const char *op, enum task_integrity_value prev,
		enum task_integrity_value tint, const char *cause, int result)
{
}
#endif

void five_audit_info(struct task_struct *task, struct file *file,
		const char *op, enum task_integrity_value prev,
		enum task_integrity_value tint, const char *cause, int result)
{
	five_audit_msg(task, file, op, prev, tint, cause, result);
}
void five_audit_err(struct task_struct *task, struct file *file,
		const char *op, enum task_integrity_value prev,
		enum task_integrity_value tint, const char *cause, int result)
{
	five_audit_msg(task, file, op, prev, tint, cause, result);
}

static void five_audit_msg(struct task_struct *task, struct file *file,
		const char *op, enum task_integrity_value prev,
		enum task_integrity_value tint, const char *cause, int result)
{
	struct audit_buffer *ab;
	struct inode *inode = NULL;
	const char *fname = NULL;
	char *pathbuf = NULL;
	char comm[TASK_COMM_LEN];
	const char *name = "";
	unsigned long ino = 0;
	char *dev = "";
	struct task_struct *tsk = task ? task : current;

	if (file) {
		inode = file_inode(file);
		fname = five_d_path(&file->f_path, &pathbuf);
	}

	ab = audit_log_start(current->audit_context, GFP_KERNEL,
			AUDIT_INTEGRITY_DATA);
	if (unlikely(!ab)) {
		pr_err("Can't get a context of audit logs\n");
		goto exit;
	}

	audit_log_format(ab, " pid=%d", task_pid_nr(tsk));
	audit_log_format(ab, " gpid=%d",
			task_pid_nr(tsk->group_leader));
	audit_log_task_context(ab);
	audit_log_format(ab, " op=");
	audit_log_string(ab, op);
	audit_log_format(ab, " cint=0x%x", tint);
	audit_log_format(ab, " pint=0x%x", prev);
	audit_log_format(ab, " cause=");
	audit_log_string(ab, cause);
	audit_log_format(ab, " comm=");
	audit_log_untrustedstring(ab, get_task_comm(comm, tsk));
	if (fname) {
		audit_log_format(ab, " name=");
		audit_log_untrustedstring(ab, fname);
		name = fname;
	}
	if (inode) {
		audit_log_format(ab, " dev=");
		audit_log_untrustedstring(ab, inode->i_sb->s_id);
		audit_log_format(ab, " ino=%lu", inode->i_ino);
		ino = inode->i_ino;
		dev = inode->i_sb->s_id;
	}
	audit_log_format(ab, " res=%d", result);
	audit_log_end(ab);

	pr_info("FIVE: pid=%d gpid=%d op='%s' cint=0x%x pint=0x%x cause=%s"
			"comm='%s' name='%s' dev='%s' ino=%lu res=%d",
		task_pid_nr(tsk), task_pid_nr(tsk->group_leader), op,
		tint, prev, cause, get_task_comm(comm, tsk),
		name, dev, ino, result);
exit:
	if (pathbuf)
		__putname(pathbuf);
}

void five_audit_hexinfo(struct file *file, const char *msg, char *data,
		size_t data_length)
{
	struct audit_buffer *ab;
	struct inode *inode = NULL;
	const unsigned char *fname = NULL;
	char *pathbuf = NULL;
	struct integrity_iint_cache *iint = NULL;

	if (file) {
		fname = five_d_path(&file->f_path, &pathbuf);
		inode = file_inode(file);
	}

	ab = audit_log_start(current->audit_context, GFP_KERNEL,
			AUDIT_INTEGRITY_DATA);
	if (unlikely(!ab)) {
		pr_err("Can't get a context of audit logs\n");
		goto exit;
	}

	if (fname) {
		audit_log_format(ab, " name=");
		audit_log_untrustedstring(ab, fname);
	}
	if (inode) {
		audit_log_format(ab, " i_version=%lu ",
				(unsigned long)inode->i_version);
		iint = integrity_inode_get(inode);
		if (iint)
			audit_log_format(ab, " cache_value=%lu ", iint->five_flags);
	}

	audit_log_string(ab, msg);
	audit_log_n_hex(ab, data, data_length);
	audit_log_end(ab);
exit:
	if (pathbuf)
		__putname(pathbuf);
}
