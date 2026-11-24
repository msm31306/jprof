/*
 * demo.c - profiling target with named threads and known CPU shares.
 *
 * Runs four threads concurrently:
 *   main    ~3 s CPU in matrix_mul()
 *   hasher  ~2 s CPU in hash_block()
 *   sorter  ~1 s CPU in sort_chunk()
 *   sleeper  sleeps (should get ~0 samples: on-CPU profiling only)
 * Expected thread shares: about 50% / 33% / 17% / 0%.
 *
 * Build: gcc -O1 -g -fno-omit-frame-pointer -pthread demo.c -o demo
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile uint64_t sink;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);	/* this thread's CPU time */
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

__attribute__((noinline)) static void matrix_mul(void)
{
	static double a[64][64], b[64][64], c[64][64];

	for (int i = 0; i < 64; i++)
		for (int j = 0; j < 64; j++)
			for (int k = 0; k < 64; k++)
				c[i][j] += a[i][k] * b[k][j];
	sink += (uint64_t)c[1][1];
}

__attribute__((noinline)) static void hash_block(void)
{
	uint64_t h = 1469598103934665603ULL;

	for (int i = 0; i < 200000; i++)
		h = (h ^ (uint64_t)i) * 1099511628211ULL;
	sink += h;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

__attribute__((noinline)) static void sort_chunk(void)
{
	static int v[4096];

	for (int i = 0; i < 4096; i++)
		v[i] = rand();
	qsort(v, 4096, sizeof(int), cmp_int);
	sink += v[0];
}

__attribute__((noinline)) static void run_for(void (*fn)(void), double cpu_s)
{
	double end = now_s() + cpu_s;

	while (now_s() < end)
		fn();
}

static void *hasher(void *arg)
{
	pthread_setname_np(pthread_self(), "hasher");
	run_for(hash_block, 2.0);
	return NULL;
}

static void *sorter(void *arg)
{
	pthread_setname_np(pthread_self(), "sorter");
	run_for(sort_chunk, 1.0);
	return NULL;
}

static void *sleeper(void *arg)
{
	pthread_setname_np(pthread_self(), "sleeper");
	sleep(3);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t t[3];
	int delay = argc > 1 ? atoi(argv[1]) : 0;

	pthread_setname_np(pthread_self(), "main");
	printf("demo pid %d\n", getpid());
	fflush(stdout);
	sleep(delay);			/* time to attach the profiler */

	pthread_create(&t[0], NULL, hasher, NULL);
	pthread_create(&t[1], NULL, sorter, NULL);
	pthread_create(&t[2], NULL, sleeper, NULL);
	run_for(matrix_mul, 3.0);
	for (int i = 0; i < 3; i++)
		pthread_join(t[i], NULL);
	return 0;
}
