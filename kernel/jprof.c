// SPDX-License-Identifier: GPL-2.0
/*
 * jprof - a jiffies-driven sampling profiler for one user process.
 *
 * Every N jiffies, a pinned timer on each CPU checks whether the task
 * it interrupted belongs to the target process. If so, it walks that
 * thread's user stack (frame pointers) and pushes a fixed-size sample
 * into the CPU's lock-free kfifo. A per-thread table aggregates counts.
 *
 *   /dev/jprof            ioctl START/STOP/INFO, non-blocking read, poll
 *   /proc/jprof/stats     session counters
 *   /proc/jprof/threads   per-thread breakdown, sorted, human readable
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/random.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kfifo.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/capability.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sort.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <asm/ptrace.h>
#ifdef CONFIG_ARM64
#include <asm/pointer_auth.h>
#endif

#include "jprof_uapi.h"

/* ------------------------------------------------------------------ */
/* Kernel-version compatibility                                        */
/* ------------------------------------------------------------------ */

/*
 * arm64 pointer authentication: code built with -mbranch-protection
 * (Ubuntu's default for packages) signs return addresses before saving
 * them, putting a signature in the unused top bits. Strip it, as perf
 * does, or the address matches no mapping.
 */
#if defined(CONFIG_ARM64) && LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define jp_strip_pac(ret)	ptrauth_strip_user_insn_pac(ret)
#elif defined(CONFIG_ARM64)
#define jp_strip_pac(ret)	ptrauth_strip_insn_pac(ret)
#else
#define jp_strip_pac(ret)	(ret)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define jp_timer_entry(var, t, field)	timer_container_of(var, t, field)
#else
#define jp_timer_entry(var, t, field)	from_timer(var, t, field)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#define jp_hrtimer_setup(t, fn)	hrtimer_setup(t, fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD)
#else
#define jp_hrtimer_setup(t, fn)	do { hrtimer_init(t, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD); \
				     (t)->function = (fn); } while (0)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
#define jp_timer_stop(t)		timer_shutdown_sync(t)
#else
#define jp_timer_stop(t)		del_timer_sync(t)
#endif

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

#define JPROF_FIFO_RECORDS	512	/* per CPU, power of two */
#define JPROF_MAX_THREADS	256
#define JPROF_HASH_BITS		6	/* 64 buckets */

enum { JP_IDLE = 0, JP_RUNNING = 1, JP_ENDED = 2 };

/* Thread states recorded by the per-tick state scan. */
enum { JP_ST_RUN, JP_ST_WAIT, JP_ST_SLEEP, JP_ST_BLOCK, JP_NSTATES };
static const char *const jp_state_label[JP_NSTATES] = {
	"running", "wait-cpu", "sleeping", "blocked"
};

/* Per-CPU state. Only this CPU's pinned timer writes here. */
struct jprof_cpu {
	struct timer_list timer;	/* clock=jiffies */
	struct hrtimer hrt;		/* clock=hrtimer */
	u64 last_ns;			/* hrtimer: ktime at previous callback */
	u64 expect_ns;			/* hrtimer: period requested for this one */
	DECLARE_KFIFO_PTR(fifo, struct jprof_sample);
	struct jprof_sample scratch;	/* sample is built here, not on the stack */
	unsigned long last_fire;	/* jiffies at previous callback */
	u64 ticks, hits, drops, walk_faults, lagged;
	u64 idle, kthread;		/* what non-matching ticks landed on */
	u64 cost_ns, cost_hit_ns, cost_max_ns;	/* self-measured callback cost */
};

/* One entry per thread seen by the sampler. */
struct jprof_thread {
	struct hlist_node node;
	pid_t tid;
	char name[TASK_COMM_LEN];
	u64 samples;
	u64 cpu_ns;			/* sum of the time each sample stands for */
	/*
	 * Ground truth for validation: the scheduler's own CPU-time counter
	 * (se.sum_exec_runtime, ns) at this thread's first and last sample,
	 * and the weight of the first sample (it covers time before the
	 * first reading, so it is excluded when comparing).
	 */
	u64 rt_first, rt_last;
	u64 first_weight_ns;
	u64 ts_first, ts_last;			/* ktime (ns) at first/last sample */
	u64 state_ns[JP_NSTATES];		/* time-weighted state scan results */
	u64 walk_faults;
	unsigned long first_seen, last_seen;	/* jiffies */
	u64 cpu_mask;				/* CPUs it was sampled on (0..63) */
};

struct jprof_session {
	int state;				/* READ_ONCE / WRITE_ONCE / cmpxchg */

	/* Target, fixed between START and teardown */
	struct pid *target;			/* the process (TGID) */
	struct pid *tid_filter;			/* NULL = all threads */
	struct pid_namespace *ns;		/* reader's namespace for numbers */
	pid_t pid_nr;
	char proc_name[JPROF_PROC_LEN];
	unsigned long interval;
	unsigned int max_depth;
	bool hr;				/* sampling clock: hrtimer (true) or jiffies */
	int scan_cpu;				/* CPU whose ticks also scan thread states */
	u64 start_ns, end_ns;

	struct jprof_cpu __percpu *pcpu;
	cpumask_t armed;			/* CPUs with a live timer */

	/* Per-thread aggregation (written in softirq) */
	spinlock_t thr_lock;
	DECLARE_HASHTABLE(threads, JPROF_HASH_BITS);
	unsigned int nthreads;
	u64 thr_overflow;

	/* Process-context side */
	struct mutex lock;			/* lifecycle + the single consumer */
	wait_queue_head_t wq;			/* poll() sleeps here */
};

/*
 * Wake the reader only once a CPU's buffer holds this many samples (or the
 * session ends), instead of on every sample. Same idea as perf's
 * wakeup_watermark: wakeups cost an IPI and a context switch, which is far
 * more than taking the sample itself. 1 = wake on every sample.
 * Runtime-tunable: /sys/module/jprof/parameters/wakeup_watermark
 */
static unsigned int wakeup_watermark = 64;
module_param(wakeup_watermark, uint, 0644);
MODULE_PARM_DESC(wakeup_watermark,
		 "samples buffered per CPU before waking the reader (default 64, 1 = every sample)");

/*
 * Sampling clock, read at each JPROF_START:
 *   hrtimer  high-resolution timer in hard-IRQ context, period = interval
 *            jiffies converted to ns, +/-25% random jitter (default)
 *   jiffies  timer-wheel timer (timer_list) in softirq context
 * Runtime-tunable: /sys/module/jprof/parameters/clock
 */
static char jp_clock[16] = "hrtimer";
module_param_string(clock, jp_clock, sizeof(jp_clock), 0644);
MODULE_PARM_DESC(clock, "sampling clock: hrtimer (default) or jiffies");

static struct jprof_session S;
static atomic_t jp_open_count = ATOMIC_INIT(0);
static struct proc_dir_entry *jp_proc_dir;

/* ------------------------------------------------------------------ */
/* Sampler: runs in softirq context. Must never sleep.                 */
/* ------------------------------------------------------------------ */

/*
 * Walk the user stack using frame pointers.
 * Frame record layout with -fno-omit-frame-pointer (same on x86-64 and
 * arm64, where it is the {x29, x30} pair):
 *     [fp + 0] = caller's saved frame pointer
 *     [fp + 8] = return address (PAC-signed on arm64; see jp_strip_pac)
 * Returns the number of entries written to ips[]. Sets *faulted if a
 * frame could not be read.
 */
static unsigned int jp_walk_user_stack(struct pt_regs *regs, u64 *ips,
				       unsigned int max, bool *faulted)
{
	struct { unsigned long next_fp, ret; } frame;
	unsigned long fp = frame_pointer(regs);
	unsigned int depth = 0;

	*faulted = false;
	ips[depth++] = instruction_pointer(regs);

	while (depth < max) {
		if (!fp || (fp & (sizeof(long) - 1)))
			break;				/* end of chain or garbage */
		if (copy_from_user_nofault(&frame, (const void __user *)fp,
					   sizeof(frame))) {
			*faulted = true;		/* unmapped / not resident */
			break;
		}
		if (!frame.ret)
			break;
		ips[depth++] = jp_strip_pac(frame.ret);
		if (frame.next_fp <= fp)
			break;				/* must move toward stack base */
		fp = frame.next_fp;
	}
	return depth;
}

/*
 * Find or create the table entry for a thread. Caller holds thr_lock.
 * Returns NULL if the table is full or memory is short (never sleeps).
 */
static struct jprof_thread *jp_thread_get(struct jprof_session *s,
					  struct task_struct *task, pid_t tid)
{
	struct jprof_thread *t;

	hash_for_each_possible(s->threads, t, node, tid)
		if (t->tid == tid)
			return t;
	if (s->nthreads >= JPROF_MAX_THREADS)
		return NULL;
	t = kzalloc(sizeof(*t), GFP_ATOMIC);	/* cannot sleep here */
	if (!t)
		return NULL;
	t->tid = tid;
	t->first_seen = jiffies;
	t->rt_first = READ_ONCE(task->se.sum_exec_runtime);
	t->ts_first = ktime_get_ns();
	strscpy(t->name, task->comm, sizeof(t->name));
	hash_add(s->threads, &t->node, tid);
	s->nthreads++;
	return t;
}

static void jp_account_thread(struct jprof_session *s, struct task_struct *cur,
			      pid_t tid, int cpu, u64 weight_ns, bool faulted)
{
	struct jprof_thread *t;
	unsigned long flags;

	/* Taken from hard-IRQ (hrtimer), softirq (timer_list) and process
	 * context (/proc reader): irqsave is the one form safe in all three. */
	spin_lock_irqsave(&s->thr_lock, flags);
	t = jp_thread_get(s, cur, tid);
	if (!t) {
		s->thr_overflow++;
		goto out;
	}
	if (!t->samples)
		t->first_weight_ns = weight_ns;	/* covers time before rt_first */
	t->samples++;
	t->cpu_ns += weight_ns;
	t->rt_last = cur->se.sum_exec_runtime;	/* updated by the tick just before us */
	t->ts_last = ktime_get_ns();
	if (faulted)
		t->walk_faults++;
	t->last_seen = jiffies;
	t->cpu_mask |= BIT_ULL(cpu & 63);
	strscpy(t->name, cur->comm, sizeof(t->name));	/* threads may rename */
out:
	spin_unlock_irqrestore(&s->thr_lock, flags);
}

/*
 * State scan (scanner CPU only): classify every thread of the target and
 * credit weight_ns to its current state. Lock-free with respect to the
 * scheduler: the thread list is walked under RCU and state is read with
 * READ_ONCE, so each reading is a snapshot that may be a moment stale;
 * over thousands of scans that averages out. Caller holds rcu_read_lock().
 */
static void jp_scan_states(struct jprof_session *s, struct task_struct *leader,
			   u64 weight_ns)
{
	struct task_struct *task;
	struct jprof_thread *t;
	unsigned long flags;
	int st;

	spin_lock_irqsave(&s->thr_lock, flags);
	for_each_thread(leader, task) {
		if (s->tid_filter && task_pid(task) != s->tid_filter)
			continue;
		switch (task_state_index(task)) {	/* index into "RSDTtXZPI" */
		case 0:					/* TASK_RUNNING */
			st = READ_ONCE(task->on_cpu) ? JP_ST_RUN : JP_ST_WAIT;
			break;
		case 2:					/* D: uninterruptible */
			st = JP_ST_BLOCK;
			break;
		case 5: case 6:				/* X, Z: exiting */
			continue;
		default:				/* S, T, t, P, I */
			st = JP_ST_SLEEP;
			break;
		}
		t = jp_thread_get(s, task, task_pid_nr_ns(task, s->ns));
		if (t) {
			t->state_ns[st] += weight_ns;
			strscpy(t->name, task->comm, sizeof(t->name));	/* may rename */
		}
	}
	spin_unlock_irqrestore(&s->thr_lock, flags);
}

static void jp_end_session(struct jprof_session *s)
{
	/* Only the first caller (any CPU, or STOP) performs the transition. */
	if (cmpxchg(&s->state, JP_RUNNING, JP_ENDED) == JP_RUNNING) {
		WRITE_ONCE(s->end_ns, ktime_get_ns());
		wake_up_interruptible(&s->wq);
	}
}

/*
 * One sampling step, shared by both clocks so they are compared fairly.
 * weight_ns = real time this sample stands for; late = the timer fired
 * noticeably after it was due. Returns false if the target has exited
 * (the caller must then not re-arm).
 */
static bool jp_sample_step(struct jprof_session *s, struct jprof_cpu *pc,
			   u64 weight_ns, bool late)
{
	struct task_struct *cur = current;	/* the task this tick interrupted */
	struct jprof_sample *smp = &pc->scratch;
	struct task_struct *leader;
	bool alive, faulted, hit = false;
	int cpu = smp_processor_id();
	u64 t0 = ktime_get_ns(), dt;		/* self-measurement starts */
	pid_t tid;

	pc->ticks++;
	if (late)
		pc->lagged++;

	/* 1. Does the target process still exist? If so, and this is the
	 *    scanner CPU, record the state of each of its threads. */
	rcu_read_lock();
	leader = pid_task(s->target, PIDTYPE_TGID);
	alive = leader != NULL;
	if (alive && cpu == READ_ONCE(s->scan_cpu))
		jp_scan_states(s, leader, weight_ns);
	rcu_read_unlock();
	if (!alive) {
		jp_end_session(s);
		return false;
	}

	/* 2. Is the interrupted task one of the target's threads? */
	if (task_tgid(cur) != s->target) {
		if (is_idle_task(cur))
			pc->idle++;		/* CPU had nothing to run */
		else if (cur->flags & PF_KTHREAD)
			pc->kthread++;		/* e.g. ksoftirqd, kworker */
		goto out;
	}
	if (s->tid_filter && task_pid(cur) != s->tid_filter)
		goto out;
	if (!cur->mm || (cur->flags & PF_EXITING))
		goto out;			/* no user address space to walk */
	pc->hits++;
	hit = true;

	/* 3. Build the sample in the per-CPU scratch buffer. */
	tid = task_pid_nr_ns(cur, s->ns);
	smp->ts_ns = t0;
	smp->tid = tid;
	smp->cpu = cpu;
	strscpy(smp->thread_name, cur->comm, sizeof(smp->thread_name));
	smp->depth = jp_walk_user_stack(task_pt_regs(cur), smp->ips,
					s->max_depth, &faulted);
	if (faulted)
		pc->walk_faults++;

	/* 4. Publish: single producer (this CPU), single consumer (reader). */
	if (!kfifo_in(&pc->fifo, smp, 1))
		pc->drops++;
	jp_account_thread(s, cur, tid, cpu, weight_ns, faulted);
	if (kfifo_len(&pc->fifo) >= max(READ_ONCE(wakeup_watermark), 1U) &&
	    wq_has_sleeper(&s->wq))
		wake_up_interruptible(&s->wq);
out:
	/* 5. Self-measurement: cost of this step, hit or miss. */
	dt = ktime_get_ns() - t0;
	pc->cost_ns += dt;
	if (hit)
		pc->cost_hit_ns += dt;
	if (dt > pc->cost_max_ns)
		pc->cost_max_ns = dt;
	return true;
}

/* clock=jiffies: timer-wheel timer, runs in softirq context. */
static void jprof_tick(struct timer_list *tl)
{
	struct jprof_cpu *pc = jp_timer_entry(pc, tl, timer);
	struct jprof_session *s = &S;
	unsigned long weight;

	if (READ_ONCE(s->state) != JP_RUNNING)
		return;				/* do not re-arm */
	/*
	 * Weight = jiffies since this CPU's previous callback: equal to the
	 * interval normally, larger when the timer ran late.
	 */
	weight = jiffies - pc->last_fire;
	pc->last_fire = jiffies;
	if (!jp_sample_step(s, pc, (u64)weight * TICK_NSEC, weight > s->interval))
		return;
	/*
	 * Re-arm. The timer wheel rounds every expiry up by one tick so that
	 * a timer can never fire early (calc_index() in kernel/time/timer.c);
	 * asking for "now + interval - 1" fires "interval" ticks after this.
	 */
	mod_timer(tl, jiffies + s->interval - 1);	/* TIMER_PINNED: stays on this CPU */
}

/* Next hrtimer period: interval jiffies in ns, uniformly jittered by +/-25%
 * so samples cannot phase-lock onto a periodic workload. */
static u64 jp_hr_period(struct jprof_session *s)
{
	u64 base = (u64)s->interval * TICK_NSEC;

	return base - base / 4 + get_random_u32_below((u32)(base / 2) + 1);
}

/* clock=hrtimer: high-resolution timer, runs in hard-IRQ context. */
static enum hrtimer_restart jprof_hrtick(struct hrtimer *ht)
{
	struct jprof_cpu *pc = container_of(ht, struct jprof_cpu, hrt);
	struct jprof_session *s = &S;
	u64 now, weight;

	if (READ_ONCE(s->state) != JP_RUNNING)
		return HRTIMER_NORESTART;
	now = ktime_get_ns();
	weight = now - pc->last_ns;		/* real ns since the previous sample */
	pc->last_ns = now;
	if (!jp_sample_step(s, pc, weight, weight > pc->expect_ns + pc->expect_ns / 2))
		return HRTIMER_NORESTART;
	pc->expect_ns = jp_hr_period(s);
	hrtimer_forward_now(ht, ns_to_ktime(pc->expect_ns));
	return HRTIMER_RESTART;
}

/* Runs on each CPU via on_each_cpu(): a PINNED hrtimer stays on the CPU
 * that starts it, so it must be started from that CPU. */
static void jp_hr_start_local(void *unused)
{
	struct jprof_cpu *pc = this_cpu_ptr(S.pcpu);

	jp_hrtimer_setup(&pc->hrt, jprof_hrtick);
	pc->last_ns = ktime_get_ns();
	pc->expect_ns = jp_hr_period(&S);
	cpumask_set_cpu(smp_processor_id(), &S.armed);
	hrtimer_start(&pc->hrt, ns_to_ktime(pc->expect_ns), HRTIMER_MODE_REL_PINNED_HARD);
}

/* ------------------------------------------------------------------ */
/* Session lifecycle (process context, called with S.lock held)        */
/* ------------------------------------------------------------------ */

/* Stop sampling but keep buffers and stats so they can still be read. */
static void jp_stop_sampling(struct jprof_session *s)
{
	int cpu;

	jp_end_session(s);			/* callbacks stop re-arming */
	for_each_cpu(cpu, &s->armed) {
		struct jprof_cpu *pc = per_cpu_ptr(s->pcpu, cpu);

		if (s->hr)
			hrtimer_cancel(&pc->hrt);	/* waits for a running callback */
		else
			jp_timer_stop(&pc->timer);
	}
	cpumask_clear(&s->armed);
	/* From here on no callback can touch the fifos or the table. */
}

/* Free everything. Sampling must already be stopped. */
static void jp_free_session(struct jprof_session *s)
{
	struct jprof_thread *t;
	struct hlist_node *tmp;
	int cpu, bkt;

	if (s->pcpu) {
		for_each_possible_cpu(cpu)
			kfifo_free(&per_cpu_ptr(s->pcpu, cpu)->fifo);
		free_percpu(s->pcpu);
		s->pcpu = NULL;
	}
	hash_for_each_safe(s->threads, bkt, tmp, t, node) {
		hash_del(&t->node);
		kfree(t);
	}
	s->nthreads = 0;
	s->thr_overflow = 0;
	put_pid(s->target);
	put_pid(s->tid_filter);
	s->target = s->tid_filter = NULL;
	if (s->ns) {
		put_pid_ns(s->ns);
		s->ns = NULL;
	}
	WRITE_ONCE(s->state, JP_IDLE);
}

/*
 * Human-readable process name: the executable's file name (e.g. "demo").
 * task->comm is not enough: it is the *main thread's* name, which changes
 * as soon as the program calls pthread_setname_np() on its main thread.
 * Falls back to comm for tasks without an executable (kernel threads).
 */
static void jp_process_name(struct task_struct *leader, char *buf, size_t len)
{
	struct mm_struct *mm = get_task_mm(leader);	/* takes a reference */
	struct file *exe;

	strscpy(buf, leader->comm, len);
	if (!mm)
		return;
	rcu_read_lock();
	exe = rcu_dereference(mm->exe_file);
	if (exe)
		strscpy(buf, exe->f_path.dentry->d_name.name, len);
	rcu_read_unlock();
	mmput(mm);					/* may sleep: outside RCU */
}

static long jp_start(struct jprof_session *s, struct jprof_start __user *uarg)
{
	struct jprof_start a;
	struct task_struct *leader, *thr;
	int cpu, ret;

	if (!perfmon_capable())
		return -EPERM;
	if (copy_from_user(&a, uarg, sizeof(a)))
		return -EFAULT;
	if (a.pid <= 0 || a.tid < 0 || a.interval < 1 ||
	    a.max_depth < 1 || a.max_depth > JPROF_MAX_DEPTH)
		return -EINVAL;
	if (READ_ONCE(s->state) == JP_RUNNING)
		return -EBUSY;
	if (READ_ONCE(s->state) == JP_ENDED) {
		/* Discard the previous run. If it ended because the target
		 * exited, some CPUs' timers may still be pending: shut them
		 * down before freeing memory their callbacks would touch. */
		jp_stop_sampling(s);
		jp_free_session(s);
	}

	/* Resolve numbers in the caller's PID namespace. */
	s->target = find_get_pid(a.pid);
	if (!s->target)
		return -ESRCH;
	ret = -ESRCH;
	rcu_read_lock();
	leader = pid_task(s->target, PIDTYPE_TGID);	/* NULL if a.pid is a TID */
	if (leader)
		get_task_struct(leader);		/* keep it valid past RCU */
	rcu_read_unlock();
	if (!leader)
		goto err;
	jp_process_name(leader, s->proc_name, sizeof(s->proc_name));
	put_task_struct(leader);

	if (a.tid) {
		s->tid_filter = find_get_pid(a.tid);
		if (!s->tid_filter)
			goto err;
		rcu_read_lock();
		thr = pid_task(s->tid_filter, PIDTYPE_PID);
		leader = pid_task(s->target, PIDTYPE_TGID);
		ret = (thr && leader && same_thread_group(thr, leader)) ? 0 : -ESRCH;
		rcu_read_unlock();
		if (ret)
			goto err;
	}

	s->ns = get_pid_ns(task_active_pid_ns(current));
	s->pid_nr = a.pid;
	s->interval = a.interval;
	s->max_depth = a.max_depth;
	if (sysfs_streq(jp_clock, "hrtimer")) {
		s->hr = true;
	} else if (sysfs_streq(jp_clock, "jiffies")) {
		s->hr = false;
	} else {
		ret = -EINVAL;			/* bad value in the clock parameter */
		goto err;
	}

	ret = -ENOMEM;
	s->pcpu = alloc_percpu(struct jprof_cpu);
	if (!s->pcpu)
		goto err;
	for_each_possible_cpu(cpu) {
		if (kfifo_alloc(&per_cpu_ptr(s->pcpu, cpu)->fifo,
				JPROF_FIFO_RECORDS, GFP_KERNEL))
			goto err;
	}

	s->start_ns = ktime_get_ns();
	s->end_ns = 0;
	WRITE_ONCE(s->state, JP_RUNNING);

	/* Arm one pinned timer per online CPU. */
	cpus_read_lock();
	s->scan_cpu = cpumask_first(cpu_online_mask);
	if (s->hr) {
		on_each_cpu(jp_hr_start_local, NULL, 1);
	} else {
		for_each_online_cpu(cpu) {
			struct jprof_cpu *pc = per_cpu_ptr(s->pcpu, cpu);

			timer_setup(&pc->timer, jprof_tick, TIMER_PINNED);
			pc->last_fire = jiffies;
			pc->timer.expires = pc->last_fire + s->interval;
			add_timer_on(&pc->timer, cpu);
			cpumask_set_cpu(cpu, &s->armed);
		}
	}
	cpus_read_unlock();

	pr_info("profiling %s [%d] every %lu jiffies (HZ=%d), clock=%s\n",
		s->proc_name, s->pid_nr, s->interval, HZ, s->hr ? "hrtimer" : "jiffies");
	return 0;

err:
	jp_free_session(s);	/* safe on a partial setup: frees what exists */
	return ret;
}

static void jp_sum_counters(struct jprof_session *s, struct jprof_info *in)
{
	int cpu;

	if (!s->pcpu)
		return;
	for_each_possible_cpu(cpu) {
		struct jprof_cpu *pc = per_cpu_ptr(s->pcpu, cpu);

		in->ticks       += READ_ONCE(pc->ticks);
		in->hits        += READ_ONCE(pc->hits);
		in->drops       += READ_ONCE(pc->drops);
		in->walk_faults += READ_ONCE(pc->walk_faults);
		in->lagged      += READ_ONCE(pc->lagged);
	}
}

static u64 jp_elapsed_ns(struct jprof_session *s)
{
	int st = READ_ONCE(s->state);

	if (st == JP_IDLE)
		return 0;
	if (st == JP_RUNNING)
		return ktime_get_ns() - s->start_ns;
	return READ_ONCE(s->end_ns) - s->start_ns;
}

static void jp_fill_info(struct jprof_session *s, struct jprof_info *in)
{
	memset(in, 0, sizeof(*in));
	in->state = READ_ONCE(s->state);
	in->hz = HZ;
	if (in->state == JP_IDLE)
		return;
	in->pid = s->pid_nr;
	in->interval = s->interval;
	strscpy(in->proc_name, s->proc_name, sizeof(in->proc_name));
	in->elapsed_ns = jp_elapsed_ns(s);
	jp_sum_counters(s, in);
}

/* ------------------------------------------------------------------ */
/* /dev/jprof file operations (process context)                        */
/* ------------------------------------------------------------------ */

static int jprof_open(struct inode *inode, struct file *f)
{
	if (atomic_cmpxchg(&jp_open_count, 0, 1))
		return -EBUSY;			/* one controller at a time */
	return stream_open(inode, f);
}

static int jprof_release(struct inode *inode, struct file *f)
{
	/* Tool exited or crashed: stop timers, keep results for /proc. */
	mutex_lock(&S.lock);
	if (S.state != JP_IDLE)
		jp_stop_sampling(&S);
	mutex_unlock(&S.lock);
	atomic_set(&jp_open_count, 0);
	return 0;
}

static long jprof_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct jprof_info info;
	long ret = 0;

	mutex_lock(&S.lock);
	switch (cmd) {
	case JPROF_START:
		ret = jp_start(&S, (struct jprof_start __user *)arg);
		break;
	case JPROF_STOP:
		if (S.state == JP_IDLE)
			ret = -EINVAL;
		else
			jp_stop_sampling(&S);
		break;
	case JPROF_INFO:
		jp_fill_info(&S, &info);
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&S.lock);
	return ret;
}

/*
 * Non-blocking by design: returns whatever whole samples are buffered,
 * -EAGAIN if none yet, 0 (EOF) once the session ended and is drained.
 */
static ssize_t jprof_read(struct file *f, char __user *buf, size_t len,
			  loff_t *off)
{
	const size_t rec = sizeof(struct jprof_sample);
	unsigned int copied;
	size_t total = 0;
	int cpu, ret = 0;

	if (len < rec)
		return -EINVAL;

	mutex_lock(&S.lock);
	if (S.state == JP_IDLE) {
		mutex_unlock(&S.lock);
		return -EINVAL;
	}
	for_each_possible_cpu(cpu) {
		struct jprof_cpu *pc = per_cpu_ptr(S.pcpu, cpu);

		if (len - total < rec)
			break;
		ret = kfifo_to_user(&pc->fifo, buf + total, len - total, &copied);
		total += copied;
		if (ret)
			break;
	}
	if (!total && !ret && READ_ONCE(S.state) == JP_RUNNING)
		ret = -EAGAIN;
	mutex_unlock(&S.lock);

	return total ? total : ret;	/* ret == 0 here means EOF */
}

static __poll_t jprof_poll(struct file *f, poll_table *wait)
{
	__poll_t mask = 0;
	int cpu;

	poll_wait(f, &S.wq, wait);

	mutex_lock(&S.lock);
	if (S.state != JP_IDLE) {
		for_each_possible_cpu(cpu) {
			if (!kfifo_is_empty(&per_cpu_ptr(S.pcpu, cpu)->fifo)) {
				mask |= EPOLLIN | EPOLLRDNORM;
				break;
			}
		}
		if (!mask && READ_ONCE(S.state) == JP_ENDED)
			mask |= EPOLLHUP;
	}
	mutex_unlock(&S.lock);
	return mask;
}

static const struct file_operations jprof_fops = {
	.owner		= THIS_MODULE,
	.open		= jprof_open,
	.release	= jprof_release,
	.read		= jprof_read,
	.poll		= jprof_poll,
	.unlocked_ioctl	= jprof_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice jprof_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "jprof",
	.fops	= &jprof_fops,
	.mode	= 0600,
};

/* ------------------------------------------------------------------ */
/* /proc/jprof: human-readable views                                   */
/* ------------------------------------------------------------------ */

static const char *const jp_state_name[] = { "idle", "running", "ended" };

/* Print x/total as a percentage with one decimal, integer math only. */
static void jp_pct(struct seq_file *m, u64 x, u64 total)
{
	u64 permille = total ? div64_u64(x * 1000, total) : 0;

	seq_printf(m, "%3llu.%llu%%", permille / 10, permille % 10);
}

static void jp_print_header(struct seq_file *m, struct jprof_info *in)
{
	u64 ms = div64_u64(in->elapsed_ns, NSEC_PER_MSEC);

	seq_printf(m, "jprof: %s [pid %d]   state: %s\n", in->proc_name, in->pid,
		   jp_state_name[in->state]);
	seq_printf(m, "interval: %u jiffies = %u ms (HZ=%u)   elapsed: %llu.%03llu s\n",
		   in->interval, jiffies_to_msecs(in->interval), in->hz,
		   ms / 1000, ms % 1000);
	seq_printf(m, "clock: %s\n\n", READ_ONCE(S.hr) ?
		   "hrtimer (hard IRQ, +/-25% jitter, ns weights)" :
		   "jiffies (timer wheel, softirq, jiffy weights)");
}

static int jprof_stats_show(struct seq_file *m, void *v)
{
	struct jprof_info in;

	mutex_lock(&S.lock);
	jp_fill_info(&S, &in);
	mutex_unlock(&S.lock);

	if (in.state == JP_IDLE) {
		seq_puts(m, "jprof: idle (no session)\n");
		return 0;
	}
	jp_print_header(m, &in);
	seq_printf(m, "ticks        %10llu   timer callbacks\n", in.ticks);
	seq_printf(m, "hits         %10llu   target was on-CPU (", in.hits);
	jp_pct(m, in.hits, in.ticks);
	seq_puts(m, " of ticks)\n");
	{
		u64 idle = 0, kthr = 0;
		int cpu;

		mutex_lock(&S.lock);
		if (S.pcpu)
			for_each_possible_cpu(cpu) {
				struct jprof_cpu *pc = per_cpu_ptr(S.pcpu, cpu);

				idle += READ_ONCE(pc->idle);
				kthr += READ_ONCE(pc->kthread);
			}
		mutex_unlock(&S.lock);
		seq_printf(m, "idle         %10llu   CPU was idle (", idle);
		jp_pct(m, idle, in.ticks);
		seq_printf(m, ")\nkthread      %10llu   a kernel thread ran, e.g. ksoftirqd (", kthr);
		jp_pct(m, kthr, in.ticks);
		seq_printf(m, ")\nother        %10llu   another user process ran\n",
			   in.ticks - in.hits - idle - kthr);
	}
	seq_printf(m, "drops        %10llu   buffer full\n", in.drops);
	seq_printf(m, "walk faults  %10llu   stack walk cut short\n", in.walk_faults);
	seq_printf(m, "lagged       %10llu   timer ran late\n", in.lagged);
	{
		u64 cost = 0, cost_hit = 0, cost_max = 0, misses, cpu_ns, pct;
		int cpu;

		mutex_lock(&S.lock);
		if (S.pcpu)
			for_each_possible_cpu(cpu) {
				struct jprof_cpu *pc = per_cpu_ptr(S.pcpu, cpu);

				cost += READ_ONCE(pc->cost_ns);
				cost_hit += READ_ONCE(pc->cost_hit_ns);
				cost_max = max(cost_max, READ_ONCE(pc->cost_max_ns));
			}
		mutex_unlock(&S.lock);
		misses = in.ticks - in.hits;
		/* CPU time available to the profiler's timers: elapsed x CPUs */
		cpu_ns = in.elapsed_ns * num_online_cpus();
		pct = cpu_ns ? div64_u64(cost * 100000, cpu_ns) : 0;	/* 1/1000 % */
		seq_puts(m, "\noverhead (self-measured callback time, ns):\n");
		seq_printf(m, "  per sample  %8llu   tick that walked a stack\n",
			   in.hits ? div64_u64(cost_hit, in.hits) : 0);
		seq_printf(m, "  per miss    %8llu   tick that found another task\n",
			   misses ? div64_u64(cost - cost_hit, misses) : 0);
		seq_printf(m, "  worst case  %8llu\n", cost_max);
		seq_printf(m, "  total CPU   %4llu.%03llu%%   of %u CPUs x elapsed\n",
			   pct / 1000, pct % 1000, num_online_cpus());
	}
	return 0;
}

/* EXACT column and signed ERROR of the estimate, over [first, last] sample. */
static void jp_print_exact(struct seq_file *m, const struct jprof_thread *t)
{
	u64 exact_ns, est_ns, exact_ms, diff, permille;

	if (t->samples < 2 || t->rt_last <= t->rt_first) {
		/* never or once on-CPU: no interval to compare over */
		seq_printf(m, " %10s %7s %10s  ", "n/a", "n/a", "n/a");
		return;
	}
	exact_ns = t->rt_last - t->rt_first;
	est_ns = t->cpu_ns - t->first_weight_ns;
	exact_ms = div64_u64(exact_ns, NSEC_PER_MSEC);
	seq_printf(m, " %6llu.%02llu s", exact_ms / 1000, (exact_ms % 1000) / 10);
	diff = est_ns > exact_ns ? est_ns - exact_ns : exact_ns - est_ns;
	permille = div64_u64(diff * 1000, exact_ns);
	seq_printf(m, " %c%3llu.%llu%%", est_ns >= exact_ns ? '+' : '-',
		   permille / 10, permille % 10);
	exact_ms = div64_u64(t->ts_last - t->ts_first, NSEC_PER_MSEC);
	seq_printf(m, " %6llu.%02llu s  ", exact_ms / 1000, (exact_ms % 1000) / 10);
}

static int jp_cmp_samples_desc(const void *a, const void *b)
{
	const struct jprof_thread *x = a, *y = b;

	if (x->cpu_ns != y->cpu_ns)
		return x->cpu_ns < y->cpu_ns ? 1 : -1;
	return x->tid - y->tid;
}

static int jprof_threads_show(struct seq_file *m, void *v)
{
	struct jprof_thread *snap, *t;
	struct jprof_info in;
	unsigned int n = 0, i;
	u64 total = 0, total_samples = 0, overflow = 0;
	unsigned long flags;
	int bkt, c;

	snap = kvmalloc_array(JPROF_MAX_THREADS, sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;

	mutex_lock(&S.lock);			/* session can't be freed under us */
	jp_fill_info(&S, &in);
	if (in.state != JP_IDLE) {
		/* irqsave: the sampler takes this lock in IRQ/softirq context. */
		spin_lock_irqsave(&S.thr_lock, flags);
		hash_for_each(S.threads, bkt, t, node)
			snap[n++] = *t;
		overflow = S.thr_overflow;
		spin_unlock_irqrestore(&S.thr_lock, flags);
	}
	mutex_unlock(&S.lock);

	if (in.state == JP_IDLE) {
		seq_puts(m, "jprof: idle (no session)\n");
		goto out;
	}

	sort(snap, n, sizeof(*snap), jp_cmp_samples_desc, NULL);
	for (i = 0; i < n; i++) {
		total += snap[i].cpu_ns;		/* share of profiled time */
		total_samples += snap[i].samples;
	}

	jp_print_header(m, &in);
	seq_printf(m, "%-8s %-16s %8s %7s %10s %10s %7s %10s  %s\n",
		   "TID", "THREAD", "SAMPLES", "SHARE", "ESTIMATE", "EXACT", "ERROR",
		   "WALL", "CPUS");
	for (i = 0; i < n; i++) {
		u64 cpu_ms = div64_u64(snap[i].cpu_ns, NSEC_PER_MSEC);
		bool first = true;

		seq_printf(m, "%-8d %-16s %8llu ", snap[i].tid, snap[i].name,
			   snap[i].samples);
		jp_pct(m, snap[i].cpu_ns, total);
		seq_printf(m, " %6llu.%02llu s", cpu_ms / 1000, (cpu_ms % 1000) / 10);
		jp_print_exact(m, &snap[i]);
		for (c = 0; c < 64; c++) {
			if (snap[i].cpu_mask & BIT_ULL(c)) {
				seq_printf(m, first ? "%d" : ",%d", c);
				first = false;
			}
		}
		seq_putc(m, '\n');
	}
	seq_printf(m, "\n%u threads, %llu samples", n, total_samples);
	if (overflow)
		seq_printf(m, ", %llu samples from untracked threads", overflow);
	seq_puts(m, "\n(SHARE and ESTIMATE are weighted by the time each sample covers.\n"
		 " EXACT = scheduler's CPU time between the thread's first and last\n"
		 " sample; ERROR compares it with ESTIMATE over the same interval.\n"
		 " WALL = real time between the thread's first and last sample)\n");

	/* Second table: what each thread was doing, from the state scan. */
	seq_printf(m, "\nTHREAD STATES (share of scanned time)\n%-8s %-16s",
		   "TID", "THREAD");
	for (c = 0; c < JP_NSTATES; c++)
		seq_printf(m, " %9s", jp_state_label[c]);
	seq_putc(m, '\n');
	for (i = 0; i < n; i++) {
		u64 sum = 0;

		for (c = 0; c < JP_NSTATES; c++)
			sum += snap[i].state_ns[c];
		seq_printf(m, "%-8d %-16s", snap[i].tid, snap[i].name);
		for (c = 0; c < JP_NSTATES; c++) {
			seq_puts(m, "    ");
			jp_pct(m, snap[i].state_ns[c], sum);
		}
		seq_putc(m, '\n');
	}
out:
	kvfree(snap);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Module init / exit                                                  */
/* ------------------------------------------------------------------ */

/*
 * /proc/jprof/raw: one line per thread, for tools (the dashboard). Values
 * are cumulative, so readers can difference two snapshots:
 *   tid cpu_ns running_ns wait_ns sleeping_ns blocked_ns name
 */
static int jprof_raw_show(struct seq_file *m, void *v)
{
	struct jprof_thread *snap, *t;
	unsigned int n = 0, i;
	unsigned long flags;
	int bkt, c;

	snap = kvmalloc_array(JPROF_MAX_THREADS, sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;
	mutex_lock(&S.lock);
	if (READ_ONCE(S.state) != JP_IDLE) {
		spin_lock_irqsave(&S.thr_lock, flags);
		hash_for_each(S.threads, bkt, t, node)
			snap[n++] = *t;
		spin_unlock_irqrestore(&S.thr_lock, flags);
	}
	mutex_unlock(&S.lock);
	for (i = 0; i < n; i++) {
		seq_printf(m, "%d %llu", snap[i].tid, snap[i].cpu_ns);
		for (c = 0; c < JP_NSTATES; c++)
			seq_printf(m, " %llu", snap[i].state_ns[c]);
		seq_printf(m, " %s\n", snap[i].name);
	}
	kvfree(snap);
	return 0;
}

static int __init jprof_init(void)
{
	int ret;

	mutex_init(&S.lock);
	spin_lock_init(&S.thr_lock);
	init_waitqueue_head(&S.wq);
	hash_init(S.threads);
	S.state = JP_IDLE;

	jp_proc_dir = proc_mkdir("jprof", NULL);
	if (!jp_proc_dir)
		return -ENOMEM;
	if (!proc_create_single("stats", 0444, jp_proc_dir, jprof_stats_show) ||
	    !proc_create_single("threads", 0444, jp_proc_dir, jprof_threads_show) ||
	    !proc_create_single("raw", 0444, jp_proc_dir, jprof_raw_show)) {
		ret = -ENOMEM;
		goto err_proc;
	}

	ret = misc_register(&jprof_misc);
	if (ret)
		goto err_proc;

	pr_info("loaded: /dev/jprof ready, HZ=%d (1 jiffy = %u us)\n",
		HZ, jiffies_to_usecs(1));
	return 0;

err_proc:
	proc_remove(jp_proc_dir);
	return ret;
}

static void __exit jprof_exit(void)
{
	misc_deregister(&jprof_misc);		/* no new opens or ioctls */
	proc_remove(jp_proc_dir);		/* waits for in-flight /proc readers */

	mutex_lock(&S.lock);
	if (S.state != JP_IDLE) {
		jp_stop_sampling(&S);
		jp_free_session(&S);
	}
	mutex_unlock(&S.lock);
	pr_info("unloaded\n");
}

module_init(jprof_init);
module_exit(jprof_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Omar");
MODULE_DESCRIPTION("jprof: jiffies-driven per-thread sampling profiler");
