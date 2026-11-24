#!/usr/bin/env python3
"""
jprof.py - user-space front end for the jprof kernel module.

Usage:  sudo python3 jprof.py <pid | process-name> [-i JIFFIES] [-t TID]
                                                    [-d SECONDS] [-n TOP]

Drives /dev/jprof (ioctl + poll + non-blocking read), then turns the raw
sampled addresses into function names with addr2line and prints, per
thread, which functions the time was spent in.

  SELF%   share of the thread's samples whose *sampled instruction* was in
          that function (time spent in the function itself)
  TOTAL%  share of the thread's samples where the function was anywhere on
          the stack (time in the function plus everything it called)

Standard library only. Struct layouts and ioctl numbers mirror
kernel/jprof_uapi.h (verified against the C header with sizeof/offsetof).
"""
import argparse
import collections
import errno
import fcntl
import os
import select
import shutil
import signal
import struct
import subprocess
import sys
import time

# ---- contract with the kernel (jprof_uapi.h) --------------------------------
MAX_DEPTH = 32
SAMPLE = struct.Struct("<QiHH16s%dQ" % MAX_DEPTH)      # 288 bytes
INFO = struct.Struct("<iIII64sQQQQQQ")                  # 128 bytes
START = struct.Struct("<iiII")                          # 16 bytes


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("j") << 8) | nr


JPROF_START = _ioc(1, 1, START.size)    # _IOW('j', 1, struct jprof_start)
JPROF_STOP = _ioc(0, 2, 0)              # _IO('j', 2)
JPROF_INFO = _ioc(2, 3, INFO.size)      # _IOR('j', 3, struct jprof_info)

assert SAMPLE.size == 288 and INFO.size == 128 and START.size == 16


# ---- target resolution -------------------------------------------------------
def resolve_target(arg):
    """Accept a PID or a process name (matched against the executable name)."""
    if arg.isdigit():
        return int(arg)
    matches = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            exe = os.path.basename(os.readlink("/proc/%s/exe" % d))
        except OSError:
            continue
        if exe == arg:
            matches.append(int(d))
    if not matches:
        sys.exit("jprof: no running process named '%s'" % arg)
    if len(matches) > 1:
        sys.exit("jprof: several processes named '%s': %s (pass a PID)"
                 % (arg, " ".join(map(str, matches))))
    return matches[0]


def snapshot_maps(pid):
    """Executable mappings: (start, end, file_offset, path). Taken before
    profiling starts, because /proc/PID/maps disappears when the target exits."""
    maps = []
    with open("/proc/%d/maps" % pid) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 6 or "x" not in parts[1]:
                continue
            if not (parts[5].startswith("/") or parts[5] == "[vdso]"):
                continue
            start, end = (int(x, 16) for x in parts[0].split("-"))
            maps.append((start, end, int(parts[2], 16), parts[5]))
    return maps


# ---- symbolization -----------------------------------------------------------
class Symbolizer:
    def __init__(self, maps):
        self.maps = maps
        self.cache = {}
        self.symtabs = {}
        self.debugfiles = {}
        self.loadsegs = {}

    def _symbols(self, path):
        """(start, end, name) of every sized function symbol, from both the
        full and the dynamic symbol table (whichever the binary still has)."""
        if path not in self.symtabs:
            syms = []
            for extra in ([], ["-D"]):
                out = subprocess.run(["nm", "-S", "--defined-only"] + extra + [path],
                                     capture_output=True, text=True).stdout
                for line in out.splitlines():
                    f = line.split()
                    if len(f) == 4 and f[2] not in "AaUuNn" and int(f[1], 16):
                        start, size = int(f[0], 16), int(f[1], 16)
                        syms.append((start, start + size, f[3].split("@")[0]))
            self.symtabs[path] = syms
        return self.symtabs[path]

    def _debugfile(self, path):
        """File to read debug info from: the binary itself if it has
        .debug_info, else a separate debug file fetched by build ID with
        debuginfod (Ubuntu ships stripped packages and serves the debug
        info at debuginfod.ubuntu.com). Falls back to the binary."""
        if path in self.debugfiles:
            return self.debugfiles[path]
        found = path
        try:
            sections = subprocess.run(["readelf", "-S", "-W", path],
                                      capture_output=True, text=True).stdout
            if ".debug_info" not in sections and shutil.which("debuginfod-find"):
                env = dict(os.environ)
                env.setdefault("DEBUGINFOD_URLS", "https://debuginfod.ubuntu.com")
                r = subprocess.run(["debuginfod-find", "debuginfo", path],
                                   capture_output=True, text=True, env=env, timeout=180)
                cand = r.stdout.strip()
                if r.returncode == 0 and os.path.isfile(cand):
                    found = cand
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.debugfiles[path] = found
        return found

    def _containing(self, path, rel):
        """Name of the function whose [start, start+size) contains rel, or None."""
        for start, end, name in self._symbols(path):
            if start <= rel < end:
                return name
        return None

    def _loads(self, path):
        """(p_offset, p_vaddr, p_filesz) of each PT_LOAD segment."""
        if path not in self.loadsegs:
            segs = []
            out = subprocess.run(["readelf", "-l", "-W", path],
                                 capture_output=True, text=True).stdout
            for line in out.splitlines():
                f = line.split()
                if f and f[0] == "LOAD":
                    segs.append((int(f[1], 16), int(f[2], 16), int(f[4], 16)))
            self.loadsegs[path] = segs
        return self.loadsegs[path]

    def _locate(self, addr):
        for start, end, off, path in self.maps:
            if start <= addr < end:
                # File offset -> link-time address (what addr2line and nm
                # use). Often equal, but not when a segment's p_vaddr
                # differs from its p_offset (common on arm64 binaries).
                foff = addr - start + off
                for p_off, p_vaddr, p_sz in self._loads(path) if path[0] == "/" else []:
                    if p_off <= foff < p_off + p_sz:
                        return path, foff - p_off + p_vaddr
                return path, foff
        return None, None

    def resolve(self, addrs):
        """Batch: one addr2line process per binary."""
        per_file = collections.defaultdict(list)
        for a in addrs:
            if a in self.cache:
                continue
            path, rel = self._locate(a)
            if path == "[vdso]":
                self.cache[a] = ("[vdso]", "kernel-provided code")
            elif path is None:
                self.cache[a] = ("[unknown 0x%x]" % a, "")
            else:
                per_file[path].append((a, rel))
        for path, items in per_file.items():
            try:
                out = subprocess.run(
                    ["addr2line", "-f", "-C", "-e", self._debugfile(path)]
                    + ["0x%x" % r for _, r in items],
                    capture_output=True, text=True, check=False).stdout.splitlines()
            except FileNotFoundError:
                sys.exit("jprof: addr2line not found (sudo apt install binutils)")
            lib = os.path.basename(path)
            for i, (a, rel) in enumerate(items):
                fn = out[2 * i] if 2 * i < len(out) else "??"
                loc = out[2 * i + 1] if 2 * i + 1 < len(out) else ""
                if loc.startswith("??"):
                    # No debug info: addr2line just names the nearest symbol
                    # *below* the address, which is wrong for internal
                    # (unexported) functions. Accept a name only if the
                    # address lies inside that symbol's size.
                    fn = (self._containing(self._debugfile(path), rel)
                          or self._containing(path, rel) or "[%s]" % lib)
                    loc = lib
                else:
                    loc = os.path.basename(loc.split(" ")[0])
                self.cache[a] = (fn, loc)
        return {a: self.cache[a] for a in addrs}


# ---- session -----------------------------------------------------------------
def record(pid, interval, tid, duration):
    fd = os.open("/dev/jprof", os.O_RDONLY | os.O_NONBLOCK)
    samples = []
    stop = {"now": False}
    signal.signal(signal.SIGINT, lambda *_: stop.update(now=True))
    try:
        fcntl.ioctl(fd, JPROF_START, START.pack(pid, tid, interval, MAX_DEPTH))
        poller = select.poll()
        poller.register(fd, select.POLLIN)
        t0 = time.monotonic()
        stopped = False
        while True:
            if not stopped and (stop["now"] or (duration and time.monotonic() - t0 >= duration)):
                fcntl.ioctl(fd, JPROF_STOP)      # stop sampling, then drain the rest
                stopped = True
            poller.poll(200)                     # sleep until data, EOF, or timeout
            try:
                buf = os.read(fd, SAMPLE.size * 256)
            except BlockingIOError:              # -EAGAIN: nothing buffered yet
                continue
            if not buf:                          # 0 bytes: session ended and drained
                break
            samples.extend(SAMPLE.iter_unpack(buf))
        info = INFO.unpack(fcntl.ioctl(fd, JPROF_INFO, bytes(INFO.size)))
        fcntl.ioctl(fd, JPROF_STOP)
    finally:
        os.close(fd)
    return samples, info


def report(samples, info, sym, top):
    (_, _, hz, interval, name, elapsed, ticks, hits, drops, faults, lagged) = info
    name = name.split(b"\0")[0].decode(errors="replace")

    per_thread = collections.OrderedDict()
    for s in samples:
        tid, depth, tname, ips = s[1], s[3], s[4], s[5:]
        th = per_thread.setdefault(tid, {"name": "", "n": 0,
                                         "self": collections.Counter(),
                                         "total": collections.Counter()})
        th["name"] = tname.split(b"\0")[0].decode(errors="replace")
        th["n"] += 1
        # ips[0] is the sampled instruction; ips[1:] are return addresses,
        # which point just past the call instruction: subtract 1 so they land
        # inside the calling function's line.
        stack = [ips[0]] + [ip - 1 for ip in ips[1:depth]]
        th["self"][stack[0]] += 1
        th["stack_addrs"] = th.get("stack_addrs", []) + [stack]

    every_addr = {a for th in per_thread.values() for st in th["stack_addrs"] for a in st}
    names = sym.resolve(every_addr)

    print()
    print("Functions per thread (SELF = in the function itself, "
          "TOTAL = function or anything it called)")
    for tid, th in sorted(per_thread.items(), key=lambda kv: -kv[1]["n"]):
        self_fn = collections.Counter()
        total_fn = collections.Counter()
        loc_of = {}
        for a, c in th["self"].items():
            fn, loc = names[a]
            self_fn[fn] += c
            loc_of.setdefault(fn, loc)
        for st in th["stack_addrs"]:
            seen = set()
            for a in st:
                fn, loc = names[a]
                loc_of.setdefault(fn, loc)
                if fn not in seen:               # count recursion once per sample
                    seen.add(fn)
                    total_fn[fn] += 1
        n = th["n"]
        print()
        print("THREAD %s [%d]  %d samples" % (th["name"], tid, n))
        print("  %6s  %6s  %-28s %s" % ("SELF%", "TOTAL%", "FUNCTION", "LOCATION"))
        ranked = sorted(total_fn, key=lambda f: (-self_fn[f], -total_fn[f]))
        for fn in ranked[:top]:
            print("  %5.1f%%  %5.1f%%  %-28s %s" % (100.0 * self_fn[fn] / n,
                                                  100.0 * total_fn[fn] / n,
                                                  fn[:28], loc_of[fn]))
    print()
    print("session: %s | %.2f s | %d jiffy = %.0f ms | %d samples | %d dropped | "
          "%d walk faults | %d of %d ticks late"
          % (name, elapsed / 1e9, interval, 1000.0 * interval / hz, len(samples),
             drops, faults, lagged, ticks))


def main():
    ap = argparse.ArgumentParser(description="jiffies-driven sampling profiler")
    ap.add_argument("target", help="PID or executable name")
    ap.add_argument("-i", "--interval", type=int, default=1, help="jiffies between samples")
    ap.add_argument("-t", "--tid", type=int, default=0, help="profile only this thread")
    ap.add_argument("-d", "--duration", type=float, default=0, help="stop after N seconds")
    ap.add_argument("-n", "--top", type=int, default=6, help="functions shown per thread")
    args = ap.parse_args()

    pid = resolve_target(args.target)
    sym = Symbolizer(snapshot_maps(pid))
    print("jprof: profiling %s (pid %d), Ctrl-C to stop early" % (args.target, pid))
    try:
        samples, info = record(pid, args.interval, args.tid, args.duration)
    except OSError as e:
        sys.exit("jprof: %s" % os.strerror(e.errno or errno.EIO))
    print()
    with open("/proc/jprof/threads") as f:       # the kernel's own per-thread table
        print(f.read().rstrip())
    report(samples, info, sym, args.top)


if __name__ == "__main__":
    main()
