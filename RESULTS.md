# jprof: verified results and findings

Platform: WSL2, kernel 6.18.54.1-microsoft-standard-WSL2, HZ=250, 4 vCPUs, GCC 13.3.

## Final end-to-end run (demo: main 3 s, hasher 2 s, sorter 1 s of CPU work, sleeper idle)

| Thread | Samples | Share (expected) | Estimate | Wall (real time) | Top function |
|---|---|---|---|---|---|
| main | 692 | 50.1% (50%) | 2.76 s | 2.76 s | matrix_mul 99.7% (demo.c:38) |
| hasher | 293 | 33.2% (33.3%) | 1.83 s | 1.83 s | hash_block 99.3% (demo.c:47) |
| sorter | 158 | 16.5% (16.7%) | 0.91 s | 0.90 s | msort_with_tmp 57.6%, cmp_int 12.7% |
| sleeper | 0 | 0% | - | - | (never on-CPU, correctly absent) |

1143 samples, 0 dropped, 0 stack-walk faults. (jiffies clock; threads that run continuously.)

## Sampling clock: jiffies vs hrtimer, bursty workload (live.c balanced: 3 workers, each busy 5 ms of every 10 ms; 4 s per run)

| Clock | Run | matrix err | hasher err | sorter err | Ticks (of 3200 max) | Late ticks |
|---|---|---|---|---|---|---|
| jiffies | 1 | -11.1% | -60.4% | -20.5% | 2711 | 691 |
| jiffies | 2 | +12.9% | -58.3% | -55.9% | 2811 | 590 |
| hrtimer | 1 | -1.6% | -4.1% | -5.9% | 3209 | 6 |
| hrtimer | 2 | -0.6% | -1.2% | -3.8% | 3200 | 2 |

Error = jprof estimate vs the scheduler's own CPU time over the same interval. hrtimer shares: 34.1/33.1/32.6% and 33.5/33.4/32.9% (expected 33.3% each).

Live dashboard, same workload, mean |LOAD - SCHED| accumulated over time:
- hrtimer: 1.9 points over 25 s (per-thread DIFF -1 / +0 / -2)
- jiffies: 13.7 points over 37 s (per-thread DIFF +12 / +11 / +17)

## Thread states (state scan each tick, hrtimer clock)

| Workload | Thread | running | wait-cpu | sleeping | Expected |
|---|---|---|---|---|---|
| demo | hasher | 100.0% | 0.0% | 0.0% | busy the whole time |
| demo | sorter | 100.0% | 0.0% | 0.0% | busy the whole time |
| demo | sleeper | 0.0% | 0.0% | 100.0% | sleeps the whole time |
| demo | main | 81.3% | 0.0% | 18.6% | 1 s startup sleep within ~3.7 s observed |
| live (balanced) | matrix / hasher / sorter | 47-48% | 0-1% | 52-53% | 50% duty cycle |
| live (balanced) | control | 0% | 0% | 100% | blocked in read() on the keyboard pipe |
| bench, 8 threads on 4 CPUs | 6 of 8 workers | 45.5-59.8% | 40.1-54.4% | 0% | ~50/50: only 4 can run at once |
| bench, 8 threads on 4 CPUs | 2 of 8 workers | 91.9-93.7% | 6.2-8.0% | 0% | got a CPU mostly to themselves after others finished |
| bench | main | 0.4% | 0.0% | 99.5% | waiting in pthread_join |

Per-thread percentages are over each thread's own lifetime, so for bench they can sum above 400%: threads that outlive others run unopposed at the end.

## Overhead (bench: 4 threads, fixed work, 7 runs per mode, median wall time; jiffies clock)

| Mode | Median | Slowdown | Self-measured: per sample / per miss / worst / total CPU |
|---|---|---|---|
| off | 0.9815 s | - | - |
| 1 jiffy, wake reader every sample | 1.0881 s | +10.86% | 5700 ns / 543 ns / 86 us / 0.087% |
| 1 jiffy, wake at 64 buffered (default) | 0.9787 s | -0.29% | 428 ns / 581 ns / 4.5 us / 0.009% |
| 2 jiffy, wake at 64 | 0.9801 s | -0.14% | |
| 5 jiffy, wake at 64 | 0.9804 s | -0.11% | |
| 10 jiffy, wake at 64 | 0.9815 s | +0.00% | |

Run-to-run spread with the profiler off: 0.973-1.031 s (about +/-3%). Live dashboard with hrtimer and the state scan: profiler CPU 0.025-0.029%.

## Failure tests on WSL (all pass, no kernel warnings)
- Bad PID -> -ESRCH
- Thread filter -> only the selected TID sampled
- TID from another process -> -ESRCH
- Controller killed with SIGKILL mid-run -> release() stops timers, results kept
- rmmod while /dev/jprof open -> refused; succeeds after close; reload works

## Second platform: Apple M4 (arm64), Ubuntu 24.04 VM

Platform: MacBook Air M4 (macOS 27), Lima 2.2.1 with Apple Virtualization (vz), 4 vCPUs, 8 GB. Guest kernel 6.8.0-142-generic aarch64, HZ=1000, GCC 13 (aarch64). Module source unchanged; built against stock Ubuntu headers with no warnings and loaded (unsigned, so the kernel is tainted, as expected).

| Check | Result |
|---|---|
| demo shares (expected 50/33.3/16.7) | 49.9 / 33.3 / 16.6%, estimate vs scheduler error 0.0% for all three |
| demo states | hasher 99.9% running, sorter 100%, sleeper 99.9% sleeping, main 81.3% running |
| bench, 8 threads on 4 CPUs | workers 49.7-61.7% running, 38.2-50.2% waiting for CPU |
| live balanced, jiffies (2 runs) | errors +2.7 / -19.8 / -19.8% and +0.2 / -2.2 / -4.1%; 2262 and 2473 late ticks (about 20%) |
| live balanced, hrtimer (2 runs) | errors -4.6 to -5.7%, shares 33.1-33.5%; 15 and 0 late ticks |
| function names | matrix_mul 99.8% (demo.c:39), hash_block 100%, msort_with_tmp 72.3% + cmp_int 11.3% |
| session | 5975 samples, 0 dropped, 0 walk faults; no kernel warnings |

### arm64: failure tests (all pass, no kernel warnings)
Bad PID -> ESRCH; TID from another process -> ESRCH; thread filter -> only the requested TID has samples; controller killed with SIGKILL -> session ended, 9 thread rows kept; rmmod refused while open, succeeds after close, reload works; second open refused (EBUSY).

### arm64: overhead (bench, 4 threads, 7 runs per mode, median)

| Mode | jiffies clock | hrtimer clock |
|---|---|---|
| off | 1.1645 s | 1.1392 s |
| 1 jiffy, wake every sample | +0.65% | -0.90% |
| 1 jiffy, wake at 64 | -1.98% | +1.22% |
| 2 jiffies, wake at 64 | -1.98% | +2.48% |
| 5 jiffies, wake at 64 | -1.99% | +0.13% |
| 10 jiffies, wake at 64 | -2.06% | +0.14% |

All modes are within run-to-run noise (single runs vary by up to 7%). Self-measured, 1 ms interval: 426-464 ns per sample with the watermark vs 1151-1625 ns waking every sample; total profiler CPU 0.033-0.098% of 4 CPUs. Unlike WSL (+10.9%), waking on every sample had no visible end-to-end cost here.

### Comparison with perf (Linux's standard profiler), same machine
Function level, demo, both tools on the same run (share of all samples):

| Thread / function | perf | jprof |
|---|---|---|
| main / matrix_mul | 49.83% | 50.0% x 99.8% = 49.9% |
| hasher / hash_block | 33.28% | 33.3% x 99.8% = 33.2% |
| sorter / msort_with_tmp | 12.28% (74% of sorter) | 74.4% of sorter |
| sorter / cmp_int | 1.92% (11.6% of sorter) | 11.1% of sorter |
| sorter / memcpy | 0.98% (5.9% of sorter) | 6.4% of sorter |

Per-thread time on the bursty workload (live balanced), against the scheduler over exactly the same 4 s window:

| Sampler | Errors (3 threads x runs) |
|---|---|
| perf per-task (counts the task's own CPU clock; reference) | -0.2% to -0.6% (2 runs) |
| perf system-wide cpu-clock (per-CPU timer, no jitter) | -15.8% to +1.5% (3 runs, mean -8%) |
| jprof hrtimer, running at the same time | -2.9% to -4.2% |

The per-task perf row shows the measurement window is aligned. perf's per-CPU timer, the same mechanism jprof uses, underestimates the bursty threads more than jprof and varies more between runs; jprof's random jitter likely explains the difference (perf's timer is strictly periodic and can fall in step with the 10 ms workload cycle).

### Real programs from Ubuntu, not rebuilt
Ubuntu 24.04 builds packages with `-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -mbranch-protection=standard` (from `dpkg-buildflags`), so jprof can unwind them.

- **xz -T4** (multithreaded compressor): 3 worker threads 99.9% running, a 4th 42.9% (last block), the main thread 100% sleeping (coordinator waiting on a condition variable). Estimates match the scheduler within 0.1%. Time is in liblzma (internal functions, no symbols) and lzma_crc64; call chains reach start_thread / thread_start.
- **python3.12**, 2 threads (recursive fib on the main thread, sha256 on a second thread): main thread 99.7% running, 70.1% of samples in `_PyEval_EvalFrameDefault` (the bytecode interpreter loop), chain up to `_PyRun_SimpleFileObject`; the second thread is 97.7% sleeping and gets 36 samples in 4 s, almost all in libcrypto. That matches a thread starved for CPython's global interpreter lock (GIL); inferred, not directly measured.

## Findings
1. **Timer wheel rounds expiries up by one tick.** mod_timer(jiffies + 1) fires after 2 jiffies. Fixed by re-arming with jiffies + interval - 1. Sample rate +70%.
2. **SHARE must be weighted by time, not sample count.** CPUs that tick late give each sample more time. Count-based share was off by up to 10 points; time-weighted share matches 3:2:1 within 0.4 points.
3. **Scheduler CPU time runs 6-11% fast on WSL2** for a single continuously busy thread: sum_exec_runtime exceeded wall-clock time, which is physically impossible. Confirmed independently with the shell `time` builtin (user 2.64 s > real 2.49 s). jprof's estimate matched wall-clock time within 0.5%.
4. **Rejected hypotheses:** ksoftirqd stealing ticks (kthread counter = 0), jiffies running slow (measured 250.00/s idle, 249.94/s under load).
5. **struct module size depends on config.** Disabling BTF in the build tree changed sizeof(struct module) and the module was refused ("Invalid module format"). The build tree must use the running kernel's exact config.
6. **GCC 13 on x86 omits frame setup in leaf functions** even with -mno-omit-leaf-frame-pointer, so the immediate caller of a leaf (run_for) is skipped in call chains. SELF attribution is unaffected. Not seen on arm64 (finding 11).
7. **Waking the reader dominated the profiler's cost.** Waking the reader on every sample slowed the target 10.9% end to end, while the callback's self-measured cost was only 0.087% of CPU: the expensive part (cross-CPU wakeup interrupts, extra context switches, a 5th runnable task on 4 CPUs) happens outside the callback. A wakeup watermark (wake only at 64 buffered samples, as perf does) cut per-sample cost 13x (5.7 us -> 0.43 us), worst case 19x, and the end-to-end slowdown to within noise. Lesson: self-timing a kernel callback does not capture indirect costs; an end-to-end benchmark is required.
8. **The jiffies timer is biased for bursty threads on virtual machines; an hrtimer is not.** Threads that alternate 5 ms running / 5 ms sleeping were mis-measured by -60% to +13% with the timer-wheel clock under Hyper-V, with the error changing sign between runs. Keeping CPUs from idling shrank the error about 4x, showing that timers on idle virtual CPUs are deferred until the CPU wakes, which pulls samples into step with the workload; about 20% of ticks were also late even on busy CPUs. A per-CPU pinned hrtimer in hard-IRQ context, with +/-25% random jitter and nanosecond weights, fired every due sample (3200 of 3200, 2-6 late) and brought the error to -0.6% to -5.9%, stable across runs.
9. **The bias is in when samples are taken, so it affects everything sampled.** Under jiffies the state scan also overstated running time (64-65% vs the actual ~48%), in step with LOAD (+11 to +17 points). Under hrtimer both the state scan (47-48% running) and LOAD (47-49%) agree with the scheduler. The two come from independent mechanisms (a state scan on one CPU vs on-CPU samples on every CPU), so their agreement is a cross-check.
10. **The jiffies bias is not specific to Hyper-V.** It reappears on Apple's hypervisor at HZ=1000: about 20% late ticks and errors up to -19.8% that change between runs. The hrtimer clock is again stable (all six readings within 1.1 points of each other).
11. **Finding 6 is x86-specific.** On arm64 GCC the leaf's caller is kept: run_for shows TOTAL 100% in every thread, so call chains are complete.
12. **The remaining hrtimer bias appears only when CPUs go idle.** Same workload (3 threads busy 5 ms of every 10 ms), 3 runs x 3 threads each:

    | Condition | Error |
    |---|---|
    | nanosleep, idle CPUs | -4.1% to -6.3% |
    | futex wakeup from another CPU, idle CPUs | -3.3% to -4.1% |
    | nanosleep, other CPUs kept busy by low-priority hogs | -1.2% to +0.1% |
    | continuous threads (demo, xz) | 0.0% to +0.1% |

    Weights are real elapsed nanoseconds, so this is not a weighting error. perf's per-CPU timer shows the same sign and a larger error on the same workload, so it is a property of timer-interrupt sampling on idle virtual CPUs rather than a jprof bug. The exact mechanism inside the idle/wakeup path is not pinned down.
13. **arm64 pointer authentication breaks naive stack walking.** Ubuntu packages sign saved return addresses (`-mbranch-protection=standard`), so the walker returned values like 0x65e0a48cfbdc33 that match no mapping. Stripping the signature in the kernel walker (`ptrauth_strip_user_insn_pac`, as perf does) fixed it: chains now reach `start_thread` and `PyEval_EvalCode`. Our own demos were unaffected because they were not built with branch protection.
14. **The tool mapped file offsets to symbol addresses assuming they are equal.** Fixed to translate through the ELF program headers (PT_LOAD p_offset -> p_vaddr), which arm64 binaries do not guarantee.

## Known limitations
- jiffies clock: biased for bursty workloads on virtual machines (findings 8, 9, 10); use clock=hrtimer (default).
- Resolution: about one jiffy (4 ms at HZ=250, 1 ms at HZ=1000) per sample.
- On-CPU stacks only; off-CPU time is measured as states (sleeping / blocked) without stacks.
- Thread states are snapshots read without the scheduler's locks; each reading can be a moment stale (averages out over many scans).
- Frame-pointer unwinding: target must be built with -fno-omit-frame-pointer (Ubuntu 24.04 packages are); leaf callers are skipped on x86.
- Stripped binaries show internal functions as [library]; debuginfod lookup is attempted but did not return debug info in testing.
- Per-thread estimates read 3-6% low for threads that sleep often while CPUs idle (finding 12).
- CPU hotplug during a session is not handled.
