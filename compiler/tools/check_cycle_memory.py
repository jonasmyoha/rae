#!/usr/bin/env python3
"""Memory flat while an app respawns its world over and over (the physics example gates).

Reads the log of a run with RAE_MEM_STATS_EVERY_MS: the app prints `<tag> cycle N` at every
respawn and `<engine> <bytes>` between respawns, the runtime prints `[mem:live] t=.. bufs=..
buf_bytes=..` samples. The samples are grouped by the cycles they fall between, not by their
wall-clock time, so a slow or loaded machine gives the same windows (fewer frames per cycle,
the same respawns):

  early window: the samples after cycle 6 up to cycle 12 (the world warmed up)
  late window:  the samples after cycle 12 to the end

It fails, saying which condition, when the run had fewer than `--cycles` respawns, the engine's
own bytes differ between respawns, a window has fewer than 4 samples, or the late window's peak
of live buffers or buffer bytes is above the early one's by more than 64 buffers or 256 KB.

Usage: check_cycle_memory.py <log> --tag '[vehicle]' --engine '[vehicle] engine bytes between respawns'
"""
import argparse
import re
import sys

EARLY_FROM, LATE_FROM = 6, 12
MIN_SAMPLES = 4
MAX_BUFS, MAX_BYTES = 64, 262144


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cycles", type=int, default=18)
    args = parser.parse_args()

    cycle = 0
    cycles = 0
    engine = []
    early = []
    late = []
    cycleLine = re.compile(re.escape(args.tag) + r" cycle (\d+)")
    for line in open(args.log, errors="replace"):
        line = line.rstrip("\n")
        found = cycleLine.match(line)
        if found:
            cycle = int(found.group(1))
            cycles += 1
        elif line.startswith(args.engine + " "):
            engine.append(line[len(args.engine) + 1:].strip())
        elif line.startswith("[mem:live]"):
            fields = dict(part.split("=", 1) for part in line.split()[1:] if "=" in part)
            sample = (int(fields.get("bufs", 0)), int(fields.get("buf_bytes", 0)))
            if cycle >= LATE_FROM:
                late.append(sample)
            elif cycle >= EARLY_FROM:
                early.append(sample)

    problems = []
    if cycles < args.cycles:
        problems.append(f"{cycles} respawn cycles, {args.cycles} needed")
    if len(engine) < args.cycles:
        problems.append(f"{len(engine)} engine byte counts, {args.cycles} needed")
    elif len(set(engine)) != 1:
        problems.append(f"the engine's bytes differ between respawns: {sorted(set(engine))}")
    if len(early) < MIN_SAMPLES or len(late) < MIN_SAMPLES:
        problems.append(
            f"{len(early)} samples in cycles {EARLY_FROM}-{LATE_FROM} and {len(late)} after "
            f"cycle {LATE_FROM}, {MIN_SAMPLES} each needed"
        )
    else:
        earlyBufs = max(s[0] for s in early)
        earlyBytes = max(s[1] for s in early)
        lateBufs = max(s[0] for s in late)
        lateBytes = max(s[1] for s in late)
        if lateBufs - earlyBufs > MAX_BUFS or lateBytes - earlyBytes > MAX_BYTES:
            problems.append(
                f"live memory grew: peak {earlyBufs} buffers / {earlyBytes} bytes in cycles "
                f"{EARLY_FROM}-{LATE_FROM}, {lateBufs} / {lateBytes} after (allowed +{MAX_BUFS} / "
                f"+{MAX_BYTES})"
            )
    if problems:
        for problem in problems:
            print(f"    {problem}")
        return 1
    print(
        f"    {cycles} cycles, {len(early)} + {len(late)} samples, "
        f"peak {max(s[1] for s in late)} bytes after cycle {LATE_FROM}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
