#!/usr/bin/env python3
"""Matched spruce or cypress captures; large files stay outside git. Every child is timed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output')
    parser.add_argument('--binary')
    parser.add_argument('--view', default='all')
    parser.add_argument('--motion', action='store_true')
    parser.add_argument('--species', choices=['spruce', 'cypress'], default='spruce')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = Path(args.binary).resolve() if args.binary else output / 'grove'
    front = '1 0 0 0 10 10 -13 3 0 0 0 0'
    views = {
        'control': ('', 73, 10, True, False),
        'named': ('', 73, 10, False, False),
        'control-front': (front, 73, 10, True, False),
        'front': (front, 73, 10, False, False),
        'close': ('1 0 0 0 10 10 -6 3 0 0 0 0', 73, 10, False, False),
        'side': ('1 -1.5707963 0 0 10 -4 1 3 0 0 0 0', 73, 10, False, False),
        'under': ('1 0 0 0.45 10 10 -3 0.7 0 0 0 0', 73, 10, False, False),
        'bare': (front, 73, 10, False, True),
        'seed1229': (front, 1229, 10, False, False),
        'seed2189': (front, 2189, 10, False, False),
        'backlit': ('1 0.9272952 0 0.07 10 18 -5 3 0 0 0 0', 73, 17.25, False, False),
        'night': (front, 73, 23, False, False),
    }
    if args.species == 'cypress':
        front = '1 0 0 0 10 -5.5 -3 4 0 0 0 0'
        views = {
            'control': ('', 73, 10, True, False),
            'named': ('', 73, 10, False, False),
            'control-front': (front, 73, 10, True, False),
            'front': (front, 73, 10, False, False),
            'close': ('1 0 0 0 10 -5.5 6 4 0 0 0 0', 73, 10, False, False),
            'side': ('1 -1.5707963 0 0 10 -19 13 4 0 0 0 0', 73, 10, False, False),
            'under': ('1 0 0 0.7 10 -5.5 9 1.4 0 0 0 0', 73, 10, False, False),
            'bare': (front, 73, 10, False, True),
            'seed1229': (front, 1229, 10, False, False),
            'seed2189': (front, 2189, 10, False, False),
            'backlit': ('1 1.5707963 0 0 10 7 13 4 0 0 0 0', 73, 17.25, False, False),
            'night': (front, 73, 23, False, False),
            'tall': ('1 0 0 0 10 -6 -25 13 0 0 0 0', 73, 10, False, False),
            'control-tall': ('1 0 0 0 10 -6 -25 13 0 0 0 0', 73, 10, True, False),
        }
    if args.view != 'all' and args.view not in views:
        parser.error('Unknown view')
    geometry = repo / '.rae/apps/138_procgen_showcase/window.138_procgen_showcase.geometry'
    saved = geometry.read_bytes() if geometry.exists() else None
    geometry.parent.mkdir(parents=True, exist_ok=True)
    geometry.write_text('100 100 1280 800\n')
    manifest = {'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
                'clock': [180, 1 / 30] if args.motion else [60, 1 / 60],
                'window': [1280, 800], 'renderScale': 1, 'wind': 1 if args.motion else 0,
                'effects': 'Preetham, exposure 1, deferred toon, CSM, SSAO, SMAA, bloom',
                'trees': 'Unchanged six specimens: 6, 6.7, 7.4, 6, 6.7, 7.4 m' if args.species == 'spruce' else 'Unchanged six cypresses: 7.5, 8.2, 8.9, 22.5, 24.6, 26.7 m; tall widths unchanged',
                'hashes': {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in [repo / f'lib/procgen/{args.species.capitalize()}.rae', repo / 'lib/procgen/FluffyFoliage.rae', repo / 'lib/procgen/SpruceControl.rae',
                                     repo / f'lib/assets/trees/{args.species}Spray.png', repo / f'lib/assets/trees/{args.species}Bark.png']},
                'captures': []}
    build = not args.binary
    try:
        for name, (camera, seed, hour, control, bare) in views.items():
            if args.view != 'all' and name != args.view:
                continue
            frames = output / f'{name}-frames'
            if args.motion:
                frames.mkdir(exist_ok=True)
            env = dict(os.environ, RAE_UI_HEADLESS='1', RAE_HEADLESS_FRAMES='180' if args.motion else '60',
                       RAE_FIXED_DT='0.033333333' if args.motion else '0.016666667',
                       RAE_WIND='1' if args.motion else '0', RAE_DEBUG_OVERLAY='0',
                       RAE_GROVE_VIEW='spruces', RAE_CAMERA_AT=camera, RAE_SPRUCE_CONTROL=str(int(control)),
                       RAE_SPRUCE_SEED=str(seed), RAE_SPRUCE_BARE=str(int(bare)),
                       RAE_SPRUCE_ISOLATE='1' if name == 'backlit' else '',
                       RAE_WALKER_HOUR=str(hour), RAE_DRS='off', RAE_RENDER_SCALE='1',
                       RAE_RUN_KEEP_BINARY=str(binary), RAE_GPU2D_SCREENSHOT=str(output / f'{name}.bmp'),
                       RAE_LANDSCAPE_WALK='1' if args.motion else '', RAE_LANDSCAPE_DISTANCE='2.5',
                       RAE_LANDSCAPE_FRAMES=str(frames) if args.motion else '')
            if args.species == 'cypress':
                env.update(RAE_GROVE_VIEW='evergreen', RAE_CYPRESS_CONTROL=str(int(control)),
                           RAE_CYPRESS_SEED=str(seed), RAE_CYPRESS_BARE=str(int(bare)), RAE_SPRUCE_ISOLATE='', RAE_SPRUCE_CONTROL='0', RAE_SPRUCE_SEED='73', RAE_SPRUCE_BARE='0', RAE_CYPRESS_ISOLATE='0' if name in ('control','named','tall','control-tall') else '1')
            command = [str(binary)]
            if build:
                command = ['compiler/bin/rae', 'run', '--project', 'examples/138_procgen_showcase',
                           'examples/138_procgen_showcase/Main.rae']
                build = False
            with (output / f'{name}.log').open('w') as log:
                subprocess.run(command, cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
            png = output / f'{name}.png'
            source = frames / 'frame179.bmp' if args.motion else output / f'{name}.bmp'
            subprocess.run(['sips', '-s', 'format', 'png', '-z', '800', '1280', str(source), '--out', str(png)],
                           check=True, timeout=20, stdout=subprocess.DEVNULL)
            if args.motion:
                subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', '30', '-i', str(frames / 'frame%d.bmp'),
                                '-vf', 'scale=1280:800', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '19',
                                str(output / f'{name}.mp4')], check=True, timeout=60)
            manifest['captures'].append({'file': png.name, 'camera': camera or ('spruces preset' if args.species == 'spruce' else 'evergreen preset'),
                'seed': seed, 'hour': hour, 'control': control, 'bare': bare, 'isolated': env.get('RAE_CYPRESS_ISOLATE') == '1' or (name == 'backlit' and args.species == 'spruce'),
                'specimens': 1 if env.get('RAE_CYPRESS_ISOLATE') == '1' or (name == 'backlit' and args.species == 'spruce') else 6, 'sha256': hashlib.sha256(png.read_bytes()).hexdigest()})
            manifest['binaryHash'] = hashlib.sha256(binary.read_bytes()).hexdigest()
            (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            print(name, flush=True)
    finally:
        if saved is None:
            geometry.unlink(missing_ok=True)
        else:
            geometry.write_bytes(saved)


if __name__ == '__main__':
    main()
