/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * jprof_uapi.h - shared contract between the jprof kernel module
 * and user space. Both sides include this file, so struct layouts
 * and ioctl numbers can never drift apart.
 */
#ifndef JPROF_UAPI_H
#define JPROF_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define JPROF_MAX_DEPTH	32	/* max frames per sample */
#define JPROF_NAME_LEN	16	/* thread name, == TASK_COMM_LEN */
#define JPROF_PROC_LEN	64	/* process (executable) name */

/* Argument of JPROF_START */
struct jprof_start {
	__s32 pid;		/* target process ID (thread group ID) */
	__s32 tid;		/* 0 = all threads, else only this thread */
	__u32 interval;		/* sampling period in jiffies, >= 1 */
	__u32 max_depth;	/* 1 .. JPROF_MAX_DEPTH */
};

/* One record returned by read(). Fixed size: 288 bytes. */
struct jprof_sample {
	__u64 ts_ns;			/* ktime_get_ns() at sample time */
	__s32 tid;			/* thread ID, in the reader's PID namespace */
	__u16 cpu;			/* CPU the sample was taken on */
	__u16 depth;			/* valid entries in ips[] */
	char  thread_name[JPROF_NAME_LEN];
	__u64 ips[JPROF_MAX_DEPTH];	/* ips[0] = sampled IP, then return addrs */
};

/* Result of JPROF_INFO */
struct jprof_info {
	__s32 pid;
	__u32 state;			/* 0 idle, 1 running, 2 ended */
	__u32 hz;			/* kernel HZ: converts jiffies to time */
	__u32 interval;
	char  proc_name[JPROF_PROC_LEN];	/* executable name, e.g. "demo" */
	__u64 elapsed_ns;
	__u64 ticks;			/* timer callbacks while running */
	__u64 hits;			/* callbacks that found the target on-CPU */
	__u64 drops;			/* samples lost: per-CPU buffer full */
	__u64 walk_faults;		/* stack walks cut short by a bad frame */
	__u64 lagged;			/* timer fired late, missed ticks skipped */
};

#define JPROF_IOC_MAGIC	'j'
#define JPROF_START	_IOW(JPROF_IOC_MAGIC, 1, struct jprof_start)
#define JPROF_STOP	_IO(JPROF_IOC_MAGIC, 2)
#define JPROF_INFO	_IOR(JPROF_IOC_MAGIC, 3, struct jprof_info)

#endif /* JPROF_UAPI_H */
