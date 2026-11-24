#!/usr/bin/env python3
"""
jprof_live.py - live terminal dashboard for the jprof kernel module.

  sudo python3 jprof_live.py --spawn ../demo/live    start the interactive
                                                     workload and drive it
                                                     with the keyboard
  sudo python3 jprof_live.py <pid | name>            watch any process
  keys: 0-3 workload mode (with --spawn), c switch sampling clock, q quit
  options: -i JIFFIES  sampling interval (default 1)
           --plain     print text frames instead of the full-screen view
           --frames N  stop after N refreshes (with --plain)

What it shows, refreshed twice a second over a sliding 2-second window:
  SHARE   each thread's share of the target's CPU time (sums to 100%)
  LOAD    each thread's use of one CPU (100% = one CPU fully busy)
  TOTAL   CPU time since profiling started
  STATE   running / sleeping / blocked, from /proc/<pid>/task/<tid>/stat
  HOT     the functions the sampled instructions were in

Where the numbers come from:
  - SHARE, LOAD and TOTAL use the kernel's time-weighted estimate
    (/proc/jprof/threads ESTIMATE column), differenced between snapshots.
  - HOT uses the raw sample stream from /dev/jprof (poll + non-blocking read).
"""
import argparse
import collections
import curses
import fcntl
import locale
import os
import re
import select
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jprof as J      # noqa: E402  (shared ioctl numbers, structs, symbols)

WINDOW_S = 2.0         # sliding window for SHARE / LOAD / HOT
REFRESH_S = 0.5
STATES = {"R": "running", "S": "sleeping", "D": "blocked (I/O)",
          "T": "stopped", "t": "stopped", "Z": "exited", "X": "exited",
          "I": "idle"}
KEYS_HELP = "[0] balanced  [1] matrix hot  [2] hasher hot  [3] sorter hot  [c] switch clock  [q] quit"
CLOCK_PARAM = "/sys/module/jprof/parameters/clock"
MODE_NAMES = {"0": "balanced", "1": "matrix hot", "2": "hasher hot", "3": "sorter hot"}


# ---- symbols: one persistent addr2line per binary ---------------------------
class LiveSymbolizer:
    def __init__(self, pid):
        self.pid = pid
        self.base = J.Symbolizer(J.snapshot_maps(pid))
        self.procs = {}
        self.cache = {}

    def refresh_maps(self):
        try:
            self.base.maps = J.snapshot_maps(self.pid)
        except OSError:
            pass                                  # target already gone

    def _addr2line(self, path):
        p = self.procs.get(path)
        if p is None:
            p = subprocess.Popen(["addr2line", "-f", "-C", "-e", path],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True, bufsize=1)
            self.procs[path] = p
        return p

    def name(self, addr):
        if addr in self.cache:
            return self.cache[addr]
        path, rel = self.base._locate(addr)
        if path is None:
            self.refresh_maps()                   # a library mapped later?
            path, rel = self.base._locate(addr)
        if path is None:
            res = ("[unknown]", "")
        elif path == "[vdso]":
            res = ("[vdso]", "")
        else:
            p = self._addr2line(path)
            p.stdin.write("0x%x\n" % rel)
            p.stdin.flush()
            fn = p.stdout.readline().strip()
            loc = p.stdout.readline().strip()
            lib = os.path.basename(path)
            if loc.startswith("??"):
                fn = self.base._containing(path, rel) or "[%s]" % lib
                loc = lib
            else:
                loc = os.path.basename(loc.split(" ")[0])
            res = (fn, loc)
        self.cache[addr] = res
        return res

    def close(self):
        for p in self.procs.values():
            try:
                p.stdin.close()
                p.wait(timeout=1)
            except Exception:
                p.kill()


# ---- data sources ------------------------------------------------------------
def read_kernel_threads():
    """{tid: (name, cpu_s, [running, wait, sleeping, blocked] s)} from
    /proc/jprof/raw (cumulative, so two snapshots can be differenced)."""
    out = {}
    try:
        with open("/proc/jprof/raw") as f:
            for line in f:
                p = line.split(" ", 6)
                if len(p) == 7:
                    out[int(p[0])] = (p[6].rstrip("\n"), int(p[1]) / 1e9,
                                      [int(x) / 1e9 for x in p[2:6]])
    except (OSError, ValueError):
        pass
    return out


def read_task_states(pid):
    """{tid: (name, state_word)} for the target's live threads."""
    out = {}
    try:
        tids = os.listdir("/proc/%d/task" % pid)
    except OSError:
        return out
    for t in tids:
        try:
            with open("/proc/%d/task/%s/stat" % (pid, t)) as f:
                stat = f.read()
            name = stat[stat.index("(") + 1:stat.rindex(")")]
            letter = stat[stat.rindex(")") + 2]
            out[int(t)] = (name, STATES.get(letter, letter))
        except (OSError, ValueError, IndexError):
            continue
    return out


def read_sched_ns(pid):
    """{tid: ns} - the scheduler's own CPU-time accounting (ground truth that
    does not depend on jprof's sampling). Uses .../schedstat (ns) when the
    kernel has it, else utime+stime from .../stat (clock ticks, 10 ms)."""
    out = {}
    try:
        tids = os.listdir("/proc/%d/task" % pid)
    except OSError:
        return out
    hz = os.sysconf("SC_CLK_TCK")
    for t in tids:
        base = "/proc/%d/task/%s/" % (pid, t)
        try:
            with open(base + "schedstat") as f:
                out[int(t)] = int(f.read().split()[0])
            continue
        except (OSError, ValueError, IndexError):
            pass
        try:
            with open(base + "stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
            out[int(t)] = (int(fields[11]) + int(fields[12])) * 1000000000 // hz
        except (OSError, ValueError, IndexError):
            continue
    return out


def read_clock():
    try:
        with open(CLOCK_PARAM) as f:
            return f.read().strip()
    except OSError:
        return "?"


def write_clock(value):
    with open(CLOCK_PARAM, "w") as f:
        f.write(value)


def read_overhead():
    try:
        with open("/proc/jprof/stats") as f:
            text = f.read()
    except OSError:
        return ""
    m = re.search(r"total CPU\s+([\d.]+%)", text)
    d = re.search(r"^drops\s+(\d+)", text, re.M)
    return "profiler CPU %s, %s dropped" % (m.group(1) if m else "?",
                                            d.group(1) if d else "?")


class Session:
    """Owns /dev/jprof: START, non-blocking drain, STOP."""
    def __init__(self, pid, interval):
        self.fd = os.open("/dev/jprof", os.O_RDONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, J.JPROF_START, J.START.pack(pid, 0, interval, J.MAX_DEPTH))
        self.poller = select.poll()
        self.poller.register(self.fd, select.POLLIN)
        self.ended = False

    def drain(self, timeout_ms):
        """Return newly available samples; sets self.ended at EOF."""
        got = []
        if self.ended:
            time.sleep(timeout_ms / 1000.0)
            return got
        self.poller.poll(timeout_ms)
        while True:
            try:
                buf = os.read(self.fd, J.SAMPLE.size * 256)
            except BlockingIOError:              # -EAGAIN: drained for now
                break
            if not buf:                          # EOF: target exited
                self.ended = True
                break
            got.extend(J.SAMPLE.iter_unpack(buf))
        return got

    def close(self):
        try:
            fcntl.ioctl(self.fd, J.JPROF_STOP)
        except OSError:
            pass
        os.close(self.fd)


# ---- model: sliding window over snapshots and samples -------------------------
class Model:
    def __init__(self, pid, label, interval, sym):
        self.pid, self.label, self.interval, self.sym = pid, label, interval, sym
        self.t0 = time.monotonic()
        self.snaps = collections.deque()          # (t, {tid: (name, est_s)}, {tid: ns})
        self.samples = collections.deque()        # (ts_ns, tid, ip0)
        self.mode = "0"
        self.clock = read_clock()
        self.err_sum = 0.0        # running |LOAD - SCHED| since last clock switch
        self.err_n = 0

    def reset(self):
        """New session (clock switched): restart the window and totals."""
        self.t0 = time.monotonic()
        self.snaps.clear()
        self.samples.clear()
        self.clock = read_clock()
        self.err_sum, self.err_n = 0.0, 0

    def add_samples(self, raw):
        for s in raw:
            self.samples.append((s[0], s[1], s[5]))

    def snapshot(self):
        now = time.monotonic()
        self.snaps.append((now, read_kernel_threads(), read_sched_ns(self.pid)))
        while len(self.snaps) > 2 and now - self.snaps[1][0] >= WINDOW_S:
            self.snaps.popleft()
        cutoff = time.monotonic_ns() - int(WINDOW_S * 1e9)
        while self.samples and self.samples[0][0] < cutoff:
            self.samples.popleft()

    def rows(self):
        """One row per thread:
        (name, share%, load%, total_s, state_now, tid, sched%, states%)"""
        (t_old, old, sold), (t_new, new, snew) = self.snaps[0], self.snaps[-1]
        dt = max(t_new - t_old, 1e-6)
        live = read_task_states(self.pid)
        tids = set(new) | set(live)
        zero = ("", 0.0, [0.0] * 4)
        delta = {t: max(new.get(t, zero)[1] - old.get(t, zero)[1], 0.0) for t in tids}
        total_delta = sum(delta.values()) or 1e-9
        rows = []
        for t in tids:
            name = live.get(t, new.get(t, ("?",)))[0]
            state = live[t][1] if t in live else "exited"
            sched = (100.0 * (snew[t] - sold[t]) / 1e9 / dt
                     if t in snew and t in sold else None)
            sd = [max(a - b, 0.0) for a, b in zip(new.get(t, zero)[2], old.get(t, zero)[2])]
            tot = sum(sd)
            states = [100.0 * x / tot for x in sd] if tot > 0 else None
            rows.append((name, 100.0 * delta[t] / total_delta, 100.0 * delta[t] / dt,
                         new.get(t, zero)[1], state, t, sched, states))
        rows.sort(key=lambda r: r[5])             # stable: thread creation order
        return rows

    def track_accuracy(self, rows):
        """Accumulate |LOAD - SCHED| for threads that are doing something."""
        for r in rows:
            load, sched = r[2], r[6]
            if sched is not None and (load > 2 or sched > 2):
                self.err_sum += abs(load - sched)
                self.err_n += 1

    def hot(self, top=2):
        """{tid: [(fn, loc, pct), ...]} over the window's samples."""
        per = collections.defaultdict(collections.Counter)
        locs = {}
        for _, tid, ip in self.samples:
            fn, loc = self.sym.name(ip)
            per[tid][fn] += 1
            locs[fn] = loc
        out = {}
        for tid, c in per.items():
            n = sum(c.values())
            out[tid] = [(fn, locs[fn], 100.0 * k / n) for fn, k in c.most_common(top)]
        return out


# ---- rendering -----------------------------------------------------------------
def render(model, width, ascii_bars, spawned, ended, fresh=True):
    """List of lines; each line is a list of (text, style) segments."""
    full, empty = ("#", ".") if ascii_bars else ("█", "░")
    barw = max(10, min(30, width - 108))
    elapsed = time.monotonic() - model.t0
    lines = []
    head = " jprof live | %s [pid %d] | clock %s | interval %d jiffy | %.1f s " % (
        model.label, model.pid, model.clock, model.interval, elapsed)
    lines.append([(head, "title")])
    if spawned:
        lines.append([(" workload mode: ", "dim"), (MODE_NAMES.get(model.mode, "?"), "hot")])
    lines.append([])
    lines.append([(" %-12s %-*s %6s %5s %5s %5s %8s  %s" % (
        "THREAD", barw + 1, "SHARE (last 2 s)", "", "LOAD", "SCHED", "DIFF", "TOTAL",
        "STATES (last 2 s, measured)"), "dim")])
    rows = model.rows() if len(model.snaps) >= 2 else []
    if rows and fresh:
        model.track_accuracy(rows)
    for name, share, load, total, state, _, sched, states in rows:
        n = int(round(share / 100.0 * barw))
        if sched is None:
            diff_txt, diff_style = "  -", "dim"
        else:
            d = load - sched
            diff_txt = "%+3.0f" % d
            diff_style = "bad" if abs(d) > 5 else "good"
        if states:
            st_txt = "run %3.0f%%  wait %3.0f%%  sleep %3.0f%%  blk %3.0f%%" % tuple(states)
        else:
            st_txt = state
        lines.append([(" %-12s " % name[:12], ""),
                      (full * n, "bar"), (empty * (barw - n), "dim"),
                      (" %5.1f%% %4.0f%%" % (share, load), ""),
                      (" %4s%%" % ("%.0f" % sched if sched is not None else "-"), "dim"),
                      (" %5s" % diff_txt, diff_style),
                      (" %6.2f s  " % total, ""),
                      (st_txt, "hot" if states and states[0] > 50 else "dim")])
    lines.append([])
    lines.append([(" HOT FUNCTIONS (last 2 s, where the sampled instruction was)", "dim")])
    hot = model.hot()
    name_of = {r[5]: r[0] for r in rows}
    for tid, fns in sorted(hot.items()):
        segs = [(" %-12s " % name_of.get(tid, str(tid))[:12], "")]
        for i, (fn, loc, pct) in enumerate(fns):
            segs.append(("%s%s" % ("   " if i else "", fn[:26]), "hot" if i == 0 else ""))
            segs.append((" %3.0f%%%s" % (pct, " (%s)" % loc if loc else ""), "dim"))
        lines.append(segs)
    if not hot:
        lines.append([(" (no samples in the window: target is idle)", "dim")])
    lines.append([])
    if model.err_n:
        lines.append([(" accuracy since clock switch: mean |LOAD - SCHED| = ", "dim"),
                      ("%.1f points" % (model.err_sum / model.err_n),
                       "bad" if model.err_sum / model.err_n > 5 else "good"),
                      (" over %.0f s (clock %s)" % (time.monotonic() - model.t0, model.clock),
                       "dim")])
    lines.append([(" LOAD = jprof's estimate of each thread's use of one CPU;"
                   " SCHED = the scheduler's own accounting; DIFF = LOAD - SCHED", "dim")])
    lines.append([(" " + read_overhead(), "dim")])
    if ended:
        lines.append([(" target exited - press q to quit", "hot")])
    elif spawned:
        lines.append([(" " + KEYS_HELP, "key")])
    else:
        lines.append([(" [q] quit", "key")])
    return lines


# ---- front ends ----------------------------------------------------------------
class Runner:
    """Session, model and workload together; handles keys for both views."""
    def __init__(self, pid, interval, model, child):
        self.pid, self.interval, self.model, self.child = pid, interval, model, child
        self.session = Session(pid, interval)

    def tick(self):
        self.model.add_samples(self.session.drain(100))

    def key(self, ch):
        """Returns False when the user asked to quit."""
        if ch in ("q", "Q"):
            return False
        if ch in ("c", "C") and not self.session.ended:
            self.switch_clock()
        elif self.child and ch in ("0", "1", "2", "3"):
            self.model.mode = ch
            try:
                self.child.stdin.write(ch.encode())
                self.child.stdin.flush()
            except (BrokenPipeError, ValueError):
                pass
        return True

    def switch_clock(self):
        """Stop the session, flip the module's clock parameter, start anew."""
        self.session.close()
        write_clock("jiffies" if read_clock() == "hrtimer" else "hrtimer")
        self.session = Session(self.pid, self.interval)
        self.model.reset()
        self.model.snapshot()

    def close(self):
        self.session.close()


def run_curses(stdscr, runner, ascii_bars, timeline=(), phase_file=None):
    """timeline: (seconds, key) pairs pressed automatically, for recorded demos."""
    model = runner.model
    t0 = time.monotonic()
    pending = sorted(timeline)

    def note(text):
        if phase_file:
            try:
                with open(phase_file, "w") as f:
                    f.write(text + "\n")
            except OSError:
                pass
    note("start")
    curses.curs_set(0)
    stdscr.nodelay(True)
    styles = {"": curses.A_NORMAL, "dim": curses.A_DIM, "title": curses.A_REVERSE,
              "hot": curses.A_BOLD, "key": curses.A_BOLD, "bar": curses.A_NORMAL}
    if curses.has_colors():
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_GREEN, -1)
        curses.init_pair(2, curses.COLOR_CYAN, -1)
        curses.init_pair(3, curses.COLOR_YELLOW, -1)
        curses.init_pair(4, curses.COLOR_RED, -1)
        styles.update(bar=curses.color_pair(1), title=curses.color_pair(2) | curses.A_REVERSE,
                      hot=curses.color_pair(3) | curses.A_BOLD,
                      good=curses.color_pair(1), bad=curses.color_pair(4) | curses.A_BOLD)
    next_draw = 0.0
    while True:
        runner.tick()
        k = stdscr.getch()
        if k == -1 and pending and time.monotonic() - t0 >= pending[0][0]:
            k = ord(pending.pop(0)[1])
            note("key " + chr(k))
        if k != -1:
            if not runner.key(chr(k) if 0 <= k < 256 else ""):
                return
            next_draw = 0.0
        if time.monotonic() >= next_draw:
            model.snapshot()
            h, w = stdscr.getmaxyx()
            stdscr.erase()
            for y, segs in enumerate(render(model, w, ascii_bars, runner.child is not None,
                                            runner.session.ended)[:h - 1]):
                x = 0
                for text, style in segs:
                    if x >= w - 1:
                        break
                    try:
                        stdscr.addstr(y, x, text[:w - 1 - x], styles.get(style, 0))
                    except curses.error:
                        pass
                    x += len(text)
            stdscr.refresh()
            next_draw = time.monotonic() + REFRESH_S


def run_plain(runner, frames, script):
    """Text frames; `script` maps frame number -> key (for unattended runs)."""
    shown = 0
    next_draw = time.monotonic() + REFRESH_S
    while frames == 0 or shown < frames:
        runner.tick()
        if time.monotonic() < next_draw:
            continue
        if shown in script and not runner.key(script[shown]):
            break
        runner.model.snapshot()
        print("\n".join("".join(t for t, _ in segs)
                        for segs in render(runner.model, 100, True, runner.child is not None,
                                           runner.session.ended)))
        print("-" * 80, flush=True)
        shown += 1
        next_draw = time.monotonic() + REFRESH_S
        if runner.session.ended:
            break


def drop_privileges():
    """Run the spawned workload as the user who invoked sudo, not as root."""
    uid, gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if gid:
        os.setgid(int(gid))
    if uid:
        os.setuid(int(uid))


def main():
    ap = argparse.ArgumentParser(description="jprof live dashboard")
    ap.add_argument("target", nargs="?", help="PID or executable name")
    ap.add_argument("--spawn", metavar="PROGRAM", help="start this program and drive it")
    ap.add_argument("-i", "--interval", type=int, default=1)
    ap.add_argument("--plain", action="store_true", help="print text frames")
    ap.add_argument("--ascii", action="store_true", help="ASCII bars")
    ap.add_argument("--frames", type=int, default=0, help="with --plain: stop after N frames")
    ap.add_argument("--script", default="",
                    help="frame:key list with --plain (e.g. 4:1,8:c); seconds:key list "
                         "in the dashboard (e.g. 10:1,20:c), for unattended runs")
    ap.add_argument("--phase-file", help="dashboard: write the last scripted key here")
    args = ap.parse_args()
    if not args.target and not args.spawn:
        ap.error("give a target or --spawn PROGRAM")

    child = None
    if args.spawn:
        child = subprocess.Popen([args.spawn], stdin=subprocess.PIPE,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 preexec_fn=drop_privileges)
        time.sleep(0.3)                           # let it map libraries, start threads
        pid, label = child.pid, os.path.basename(args.spawn)
    else:
        pid = J.resolve_target(args.target)
        label = args.target

    sym = LiveSymbolizer(pid)
    model = Model(pid, label, args.interval, sym)
    runner = Runner(pid, args.interval, model, child)
    model.snapshot()
    ascii_bars = args.ascii or "utf" not in (locale.getpreferredencoding() or "").lower()
    try:
        if args.plain:
            script = {int(a): b for a, b in (p.split(":") for p in args.script.split(",") if p)}
            run_plain(runner, args.frames, script)
        else:
            locale.setlocale(locale.LC_ALL, "")
            timeline = [(float(a), b) for a, b in
                        (p.split(":") for p in args.script.split(",") if p)]
            curses.wrapper(run_curses, runner, ascii_bars, timeline, args.phase_file)
    except KeyboardInterrupt:
        pass
    finally:
        runner.close()
        sym.close()
        if child and child.poll() is None:
            try:
                child.stdin.write(b"q")
                child.stdin.close()
                child.wait(timeout=2)
            except Exception:
                child.kill()


if __name__ == "__main__":
    main()
