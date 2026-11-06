# The summary of a profile's runs, one block a place, for tools/ios/profile-ios.sh and
# tools/profile-desktop.sh, so a phone's and a desktop's read the same.
#
#   python3 tools/profile-summary.py <folder> <summary first> <summary middle>
#
# <folder> holds places.txt ("name|..." lines) and <name>.log. Medians are over each run after
# its first 15 s (loading). The two others are "[prefix]|text" lines (profile-ios.sh's
# SUMMARY_FIRST and SUMMARY_MIDDLE).
import re
import statistics as st
import sys

out = sys.argv[1]


# "[prefix]|text" lines: (the prefix, the text a line must hold, its label)
def kinds(spec):
    found = []
    for line in spec.splitlines():
        if '|' not in line:
            continue
        prefix, text = line.split('|', 1)
        found.append((prefix, text, prefix.strip('[]')))
    return found


firsts, middles = kinds(sys.argv[2]), kinds(sys.argv[3])


def med(xs):
    return st.median(xs) if xs else float('nan')


print('What to trust (docs/profiling.md): the frame times, the cpu time and the [draws] counts. The gpu')
print('slots are when timestamps landed, not what a pass costs: work and idle time land in other slots,')
print('so a slot can read 31 ms for a pass whose removal saves 1 ms. To learn a cost, use RAE_AB.')
for line in open(f'{out}/places.txt'):
    name = line.split('|')[0].strip()
    if not name:
        continue
    try:
        lines = open(f'{out}/{name}.log', errors='replace').read().splitlines()
    except FileNotFoundError:
        print(name, 'no log')
        continue
    frames = [l for l in lines if l.startswith('[frames]')][3:]
    gpu = [l for l in lines if l.startswith('[gpu passes] average')][3:]
    timing = [l for l in lines if l.startswith('[frame] ') and 'in flight' in l][2:]
    if not frames:
        print(name, 'no [frames] lines')
        continue
    fm = [float(re.search(r'median ([\d.]+)', l).group(1)) for l in frames]
    fp = [float(re.search(r'p95 ([\d.]+)', l).group(1)) for l in frames]
    over = sum(int(re.search(r'(\d+) over', l).group(1)) for l in frames)
    count = sum(int(re.search(r'\] (\d+) in', l).group(1)) for l in frames)
    scales = sorted(set(re.search(r'scale ([\d.]+)', l).group(1) for l in frames))
    thermal = sorted(set(re.search(r'thermal (\d)', l).group(1) for l in frames))
    passes = {}
    for l in gpu:
        for k, v in re.findall(r'([a-z][a-z0-9 :()/,A-Z-]*?) (\d+)(?: \||$)', l.split('us: ', 1)[1]):
            passes.setdefault(k.strip(), []).append(int(v))
    pm = {k: med(v) / 1000 for k, v in passes.items()}
    print(f'## {name}: {count} frames, median {med(fm)} ms, p95 {med(fp)} ms, {over} over budget, scale {scales}, thermal {thermal}')
    if timing:
        cpu = [float(re.search(r'cpu ([\d.]+)', l).group(1)) for l in timing]
        acquire = [float(re.search(r'acquire ([\d.]+)', l).group(1)) for l in timing]
        print(f'   cpu {med(cpu):.1f} ms (max {max(cpu):.1f}), waiting for the drawable {med(acquire):.1f} ms')
    # The last frame logged, by pass; every cascade draws the same today, so one is enough
    draws = {}
    for l in lines:
        found = re.match(r'\[draws\] ([a-z -]+?)(?: \d+)?: (.*)', l)
        if found:
            draws[found.group(1)] = found.group(2)
    for draw_pass, text in draws.items():
        print(f'   drew, {draw_pass}: {text}')
    print('   gpu slots, ms (not costs): ' + ', '.join(f'{k} {v:.2f}' for k, v in pm.items() if k != 'total')
          + f'; total {pm.get("total", 0):.2f}')
    for prefix, text, label in firsts:
        found = [l for l in lines if l.startswith(prefix) and text in l]
        if found:
            print(f'   {label} at start:', found[0].split('] ', 1)[1])
    for prefix, text, label in middles:
        found = [l for l in lines if l.startswith(prefix) and text in l]
        if found:
            print(f'   {label}:', found[len(found) // 2].split('] ', 1)[1])
