#!/usr/bin/env python3
"""Measure old/candidate flower meshes in the same forest binary and camera."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--species', choices=['daisy', 'harebell'], default='daisy')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    geometry = repo/'.rae/apps/143_walker_forest/window.143_walker_forest.geometry'
    saved = geometry.read_bytes() if geometry.exists() else None
    geometry.parent.mkdir(parents=True, exist_ok=True)
    geometry.write_text('100 100 1280 800\n')
    results = {}
    try:
        for label, binary in [('control', args.binary), ('candidate', args.binary)]:
            env = dict(os.environ, RAE_UI_HEADLESS='1', RAE_HEADLESS_FRAMES='900',
                       RAE_FIXED_DT='0.016666667', RAE_WIND='1.5', RAE_WALKER_HOUR='10',
                       RAE_CAMERA_AT='1 1.5708 0.24 -0.38 4.2 -15.4 5.4 1.745 0 0 0 0',
                       **{'RAE_DAISY_CONTROL' if args.species == 'daisy' else 'RAE_HAREBELL_CONTROL': '1' if label == 'control' else '0'},
                       RAE_DRS='off', RAE_RENDER_SCALE='1', RAE_DEBUG_OVERLAY='0',
                       RAE_GPU_PASS_TIMING='1', RAE_FRAME_TIMING='1', RAE_GRASS_REPORT='1',
                       RAE_WGPU_REPORT='1', RAE_LANDSCAPE_WALK='', RAE_LANDSCAPE_FRAMES='')
            env.pop('RAE_GPU2D_SCREENSHOT', None)
            env.pop('RAE_PROFILE', None)
            path = args.output/f'cost-{label}.log'
            with path.open('w') as log:
                subprocess.run(['perl', '-e', 'alarm shift; exec @ARGV', '150', '/usr/bin/time', '-l',
                                str(binary.resolve())], cwd=repo, env=env, stdout=log,
                               stderr=subprocess.STDOUT, check=True, timeout=160)
            text = path.read_text()
            windows = []
            for line in text.splitlines():
                if line.startswith('[gpu passes]'):
                    windows.append({key: int(value) for key, value in
                                    (part.strip().rsplit(' ', 1) for part in line.split('us: ', 1)[1].split('|'))})
            timing = re.search(r'([\d.]+) real\s+([\d.]+) user\s+([\d.]+) sys', text)
            results[label] = {'binarySha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                              'gpuWindowsMicroseconds': windows,
                              'processSeconds': dict(zip(['real', 'user', 'system'], map(float, timing.groups()))),
                              'peakRssBytes': int(re.search(r'(\d+)  maximum resident set size', text)[1]),
                              'observations': [line for line in text.splitlines() if
                                               line.startswith(('[grass-study]', '[frame]', '[wgpu-report'))]}
            print(label, flush=True)
        (args.output/'costs.json').write_text(json.dumps(results, indent=2)+'\n')
    finally:
        if saved is None:
            geometry.unlink(missing_ok=True)
        else:
            geometry.write_bytes(saved)


if __name__ == '__main__':
    main()
