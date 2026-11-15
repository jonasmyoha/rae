#!/usr/bin/env python3
"""Memory growth read from a lib/Profile capture (docs/perf-tools-design.md section 4.7).

    python3 tools/perf-memory-growth.py <trace.json> [--warmup SECONDS]
        [--max-buffers N] [--max-buffer-bytes N]

Prints each memory counter of the capture (mem.buffers, mem.buffer bytes, mem.strings, mem.rss kb,
the GPU tables' counts and bytes): its value once the warm-up is over (default 20 s after the
first sample), its last value, and the growth per minute between them. With a limit, exits 1 when
the live buffers or their bytes grew by more than it after the warm-up: the check the 121 memory
test makes from its [mem:live] lines, from a capture instead."""
import argparse
import json
import sys

WATCHED = ("mem.", "gpu.buffers", "gpu.textures", "gpu.buffer kb", "gpu.texture kb",
           "gpu.app.buffers", "gpu.app.textures", "gpu.app.buffer kb", "gpu.app.texture kb")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace")
    parser.add_argument("--warmup", type=float, default=20.0)
    parser.add_argument("--max-buffers", type=int, default=None)
    parser.add_argument("--max-buffer-bytes", type=int, default=None)
    args = parser.parse_args()
    with open(args.trace) as handle:
        events = json.load(handle)["traceEvents"]
    series = {}
    for event in events:
        if event.get("ph") != "C":
            continue
        name = event["name"]
        if not name.startswith(WATCHED) or "frame allocations" in name:
            continue
        series.setdefault(name, []).append((float(event["ts"]) / 1e6, event["args"]["v"]))
    if not series:
        print("no memory counters in the capture (a Profile capture writes them each tick)")
        return 1
    growth = {}
    for name in sorted(series):
        samples = sorted(series[name])
        start = samples[0][0]
        after = [sample for sample in samples if sample[0] >= start + args.warmup] or samples[-1:]
        base_time, base = after[0]
        last_time, last = samples[-1]
        minutes = max(last_time - base_time, 1e-9) / 60.0
        growth[name] = last - base
        print(f"{name:28s} after warm-up {base:>12} last {last:>12} "
              f"growth {last - base:>10} ({(last - base) / minutes:,.1f} per minute over {last_time - base_time:.0f} s)")
    failed = False
    if args.max_buffers is not None and growth.get("mem.buffers", 0) > args.max_buffers:
        print(f"FAIL: live buffers grew by {growth['mem.buffers']} (limit {args.max_buffers})")
        failed = True
    if args.max_buffer_bytes is not None and growth.get("mem.buffer bytes", 0) > args.max_buffer_bytes:
        print(f"FAIL: live buffer bytes grew by {growth['mem.buffer bytes']} (limit {args.max_buffer_bytes})")
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
