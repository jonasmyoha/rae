#!/usr/bin/env python3
# The 106 gate's idle check: once loading has ended, a frame runs no app binding system
# (examples/106_mobile_ui/observeSystem/AppObservation.rae). It reads the `[loop N]` lines of a
# paused run with RAE_UI_DEBUG_LOOP=1.
#
#   assert-idle-frames.py <log> [--min-frames N]
#
# While images load (`assets=true`) each one moves the texture registry's revision, so the systems
# that watch it run, as they should. How long that takes depends on the machine and on the covers
# in the local app_cache, so the check starts after the last loading frame, not at a fixed time.
# The frame after that one still sees the last image's revision, so it is skipped too. Exit 1 when a
# later frame runs a system, or when too few frames came after loading to say anything.
import re
import sys

LINE = re.compile(r'^\[loop (\d+)\] uiActions=.* assets=(true|false) .*appSystems=(\d+)(.*)$')


def main(argv):
    if len(argv) < 2:
        print('usage: assert-idle-frames.py <log> [--min-frames N]')
        return 2
    wanted = 30
    if '--min-frames' in argv:
        wanted = int(argv[argv.index('--min-frames') + 1])
    text = open(argv[1], errors='replace').read().replace('\r', '\n')
    frames = []
    for line in text.split('\n'):
        found = LINE.match(line)
        if found:
            frames.append((int(found.group(1)), found.group(2) == 'true', int(found.group(3)), found.group(4).strip()))
    if not frames:
        print('NOT IDLE: no [loop] lines (is RAE_UI_DEBUG_LOOP=1 set?)')
        return 1
    last_loading = -1
    for at, frame in enumerate(frames):
        if frame[1]:
            last_loading = at
    settled = frames[last_loading + 2:]
    if len(settled) < wanted:
        print(f'NOT IDLE: only {len(settled)} frames after loading ended at frame {frames[last_loading][0]}, wanted {wanted}')
        return 1
    busy = [frame for frame in settled if frame[2] != 0]
    if busy:
        print(f'NOT IDLE: {len(busy)} of {len(settled)} frames after loading ran app systems; frame {busy[0][0]}: {busy[0][2]} {busy[0][3]}')
        return 1
    print(f'idle: {len(settled)} frames after loading ended at frame {frames[last_loading][0]} ran no app system')
    return 0


sys.exit(main(sys.argv))
