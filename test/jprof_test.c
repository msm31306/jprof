/*
 * jprof_test.c - minimal test client for the jprof module.
 * Usage: jprof_test <pid> [interval_jiffies] [max_seconds] [tid]
 * Starts a session, drains samples with poll()+non-blocking read()
 * until the target exits, then prints per-thread counts and INFO.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include "../kernel/jprof_uapi.h"

#define MAXT 64
static struct { int tid; char name[16]; long n; unsigned long long ip0, ip1; int depth; } th[MAXT];
static int nth;

static void account(const struct jprof_sample *s)
{
	int i;

	for (i = 0; i < nth; i++)
		if (th[i].tid == s->tid)
			break;
	if (i == nth && nth < MAXT) {
		th[nth].tid = s->tid;
		th[nth].ip0 = s->ips[0];
		th[nth].ip1 = s->depth > 1 ? s->ips[1] : 0;
		th[nth].depth = s->depth;
		nth++;
	}
	if (i < MAXT) {
		th[i].n++;
		memcpy(th[i].name, s->thread_name, 16);
	}
}

int main(int argc, char **argv)
{
	static struct jprof_sample buf[256];
	struct jprof_start st = { .tid = 0, .interval = 1, .max_depth = 32 };
	struct jprof_info in;
	long total = 0, eagain = 0, reads = 0;
	int fd, max_s = 0;
	time_t t0 = time(NULL);

	if (argc < 2) {
		fprintf(stderr, "usage: %s <pid> [interval]\n", argv[0]);
		return 2;
	}
	st.pid = atoi(argv[1]);
	if (argc > 2)
		st.interval = atoi(argv[2]);
	if (argc > 3)
		max_s = atoi(argv[3]);
	if (argc > 4)
		st.tid = atoi(argv[4]);

	setvbuf(stdout, NULL, _IONBF, 0);
	fd = open("/dev/jprof", O_RDONLY);
	if (fd < 0) { perror("open /dev/jprof"); return 1; }
	if (open("/dev/jprof", O_RDONLY) >= 0) { fprintf(stderr, "FAIL: second open allowed\n"); }
	else printf("ok: second open rejected (%s)\n", strerror(errno));
	if (ioctl(fd, JPROF_START, &st)) { perror("JPROF_START"); return 1; }

	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t r;

		if (max_s && time(NULL) - t0 >= max_s) {
			printf("time limit: STOP, then drain remaining samples\n");
			ioctl(fd, JPROF_STOP);
			max_s = 0;
		}
		poll(&p, 1, 200);
		r = read(fd, buf, sizeof(buf));
		reads++;
		if (r > 0) {
			for (size_t i = 0; i < r / sizeof(buf[0]); i++)
				account(&buf[i]);
			total += r / sizeof(buf[0]);
		} else if (r == 0) {
			printf("EOF: target exited, buffers drained\n");
			break;
		} else if (errno == EAGAIN) {
			eagain++;
		} else {
			perror("read");
			break;
		}
	}

	ioctl(fd, JPROF_INFO, &in);
	printf("info: %s pid=%d state=%u hz=%u elapsed=%.3fs ticks=%llu hits=%llu drops=%llu faults=%llu lagged=%llu\n",
	       in.proc_name, in.pid, in.state, in.hz, in.elapsed_ns / 1e9, in.ticks, in.hits,
	       in.drops, in.walk_faults, in.lagged);
	printf("read: %ld samples over %ld reads (%ld EAGAIN)\n", total, reads, eagain);
	for (int i = 0; i < nth; i++)
		printf("  tid %-6d %-16s %6ld samples  first: depth=%d ip=%#llx ret=%#llx\n",
		       th[i].tid, th[i].name, th[i].n, th[i].depth, th[i].ip0, th[i].ip1);
	ioctl(fd, JPROF_STOP);
	close(fd);
	return 0;
}
