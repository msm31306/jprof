/*
 * live.c - interactive workload for the jprof live dashboard.
 *
 * Threads:
 *   control  reads single-key commands from stdin (blocked in read())
 *   matrix   matrix_mul()   \
 *   hasher   hash_block()    > each busy for a "duty" % of every 10 ms slot
 *   sorter   sort_chunk()   /
 *
 * Commands (one character each, from stdin):
 *   0      balanced: every worker at 50% of a CPU
 *   1/2/3  make matrix / hasher / sorter hot (100%), others drop to 15%
 *   q      quit (also on EOF)
 *
 * Duty is enforced against the wall clock (CLOCK_MONOTONIC), so a worker
 * at 100% keeps one CPU fully busy and one at 15% uses ~1.5 ms per 10 ms.
 *
 * Build: gcc -O1 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
 *            -pthread -Wall live.c -o live
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define SLOT_NS		10000000ULL		/* 10 ms duty-cycle slot */
#define NWORKERS	3

static volatile int duty[NWORKERS] = { 50, 50, 50 };
static volatile uint64_t sink;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Each work function is a short chunk (tens of microseconds), so the duty
 * cycle can be enforced finely. */
__attribute__((noinline)) static void matrix_mul(void)
{
	static double a[32][32], b[32][32], c[32][32];

	for (int i = 0; i < 32; i++)
		for (int j = 0; j < 32; j++)
			for (int k = 0; k < 32; k++)
				c[i][j] += a[i][k] * b[k][j];
	sink += (uint64_t)c[1][1];
}

__attribute__((noinline)) static void hash_block(void)
{
	uint64_t h = 1469598103934665603ULL;

	for (int i = 0; i < 20000; i++)
		h = (h ^ (uint64_t)i) * 1099511628211ULL;
	sink += h;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

__attribute__((noinline)) static void sort_chunk(void)
{
	static __thread int v[512];

	for (int i = 0; i < 512; i++)
		v[i] = rand();
	qsort(v, 512, sizeof(int), cmp_int);
	sink += v[0];
}

static void (*const work[NWORKERS])(void) = { matrix_mul, hash_block, sort_chunk };
static const char *const names[NWORKERS] = { "matrix", "hasher", "sorter" };

static void *worker(void *arg)
{
	int i = (int)(intptr_t)arg;
	uint64_t slot = now_ns();

	pthread_setname_np(pthread_self(), names[i]);
	for (;;) {
		uint64_t busy_end = slot + SLOT_NS * duty[i] / 100;
		uint64_t t;

		while (now_ns() < busy_end)
			work[i]();
		slot += SLOT_NS;
		t = now_ns();
		if (slot > t) {
			struct timespec d = { 0, (long)(slot - t) };

			nanosleep(&d, NULL);
		} else {
			slot = t;		/* fell behind: start a fresh slot */
		}
	}
	return NULL;
}

static void set_mode(char c)
{
	if (c == '0') {
		for (int j = 0; j < NWORKERS; j++)
			duty[j] = 50;
	} else if (c >= '1' && c <= '0' + NWORKERS) {
		for (int j = 0; j < NWORKERS; j++)
			duty[j] = (j == c - '1') ? 100 : 15;
	}
}

int main(void)
{
	pthread_t t[NWORKERS];
	char c;

	pthread_setname_np(pthread_self(), "control");
	for (int i = 0; i < NWORKERS; i++)
		pthread_create(&t[i], NULL, worker, (void *)(intptr_t)i);

	while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q')
		set_mode(c);			/* this thread blocks in read() */
	return 0;				/* exit() ends all threads */
}
