#!/usr/bin/env python3
"""Find example files that are copies of another example's file or of a lib/
file (compiler/tools/example-copy-gate.sh; AGENTS.md "Example families").

Two files are a copy pair when they share at least MIN_SHARED meaningful lines
and those lines are at least SHARE_OF_SMALLER of the smaller file. A line is
compared trimmed; very short lines (braces, `ret true`) and lines found in
more than COMMON_CAP files (shared boilerplate such as `open ecs/Entity`) do
not count. Pairs listed in the allowlist, with a reason, pass; every other
pair fails, and so does an allowlist entry that no longer matches (the list
only shrinks).

  example_copy_check.py <repo root> <allowlist> [--list]
"""
import collections
import os
import subprocess
import sys

MIN_SHARED = 40
SHARE_OF_SMALLER = 0.5
MIN_LINE_LENGTH = 10
COMMON_CAP = 12


def tracked_files(root):
    out = subprocess.run(
        ["git", "-C", root, "ls-files", "--cached", "--others", "--exclude-standard", "examples", "lib"],
        capture_output=True, text=True, check=True).stdout.split("\n")
    return [f for f in out if f.endswith((".rae", ".wgsl"))]


def meaningful_lines(path):
    lines = collections.Counter()
    with open(path, encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if len(line) >= MIN_LINE_LENGTH:
                lines[line] += 1
    return lines


def copy_pairs(root):
    files = tracked_files(root)
    contents = {f: meaningful_lines(os.path.join(root, f)) for f in files}
    holders = collections.defaultdict(list)
    for name, lines in contents.items():
        for line in lines:
            holders[line].append(name)
    shared = collections.Counter()
    for line, names in holders.items():
        if len(names) < 2 or len(names) > COMMON_CAP:
            continue
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                a, b = sorted((names[i], names[j]))
                if not a.startswith("examples/") and not b.startswith("examples/"):
                    continue
                shared[(a, b)] += min(contents[a][line], contents[b][line])
    pairs = {}
    for (a, b), count in shared.items():
        smaller = min(sum(contents[a].values()), sum(contents[b].values()))
        if count >= MIN_SHARED and smaller > 0 and count >= SHARE_OF_SMALLER * smaller:
            pairs[(a, b)] = (count, smaller)
    return pairs


def read_allowlist(path):
    allowed = {}
    if not os.path.exists(path):
        return allowed
    with open(path, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            body, _, reason = line.partition("#")
            parts = body.split()
            if len(parts) != 2 or not reason.strip():
                print(f"allowlist line {number}: want `<file> <file>  # reason`: {line}")
                sys.exit(2)
            allowed[tuple(sorted(parts))] = reason.strip()
    return allowed


def main():
    root, allowlist = sys.argv[1], sys.argv[2]
    pairs = copy_pairs(root)
    if "--list" in sys.argv:
        for (a, b), (count, smaller) in sorted(pairs.items()):
            print(f"{a} {b}  # {count} of {smaller} lines shared")
        return 0
    allowed = read_allowlist(allowlist)
    failures = 0
    for (a, b), (count, smaller) in sorted(pairs.items()):
        if (a, b) not in allowed:
            print(f"COPY: {a} and {b} share {count} of {smaller} lines")
            failures += 1
    for (a, b) in sorted(allowed):
        if (a, b) not in pairs:
            print(f"STALE: {a} {b} is in the allowlist but no longer a copy: remove it")
            failures += 1
    if failures:
        print(f"{failures} problem(s). Move the shared code into lib/ and use it from every "
              "example (AGENTS.md, Example families); a copy that must stay goes in the "
              "allowlist with its reason.")
        return 1
    print(f"{len(pairs)} allowed copy pair(s), no new ones")
    return 0


if __name__ == "__main__":
    sys.exit(main())
