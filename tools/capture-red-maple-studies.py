#!/usr/bin/env python3
"""Matched red maple studies in the real grove. Large output stays outside git.

Uses existing free-camera controls. Optional isolation removes occluders but
keeps the first grove specimen at (4.2, 4, 0). Every child has a timeout.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output')
    parser.add_argument('--binary', help='Reuse a previously built grove binary')
    parser.add_argument('--view', default='all')
    parser.add_argument('--wind', type=float, default=0)
    parser.add_argument('--motion', action='store_true', help='Six-second fixed walk, BMP frames and MP4')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = Path(args.binary).resolve() if args.binary else output / 'grove'
    front = '1 0 0 0 10 4.2 -11 3.1 0 0 0 0'
    side = '1 -1.5707963 0 0 10 -10.8 4 3.1 0 0 0 0'
    under = '1 0 0 0.47 10 4.2 -2 1.3 0 0 0 0'
    views = {
        'control-front': (front, 605, 5.8, True, False, True, 10),
        'control-bare': (front, 605, 5.8, True, True, True, 10),
        'front': (front, 605, 5.8, False, False, True, 10),
        'side': (side, 605, 5.8, False, False, True, 10),
        'under': (under, 605, 5.8, False, False, True, 10),
        'bare-front': (front, 605, 5.8, False, True, True, 10),
        'bare-side': (side, 605, 5.8, False, True, True, 10),
        'seed311': (front, 311, 4.5, False, False, True, 10),
        'seed311-bare': (front, 311, 4.5, False, True, True, 10),
        'seed1021': ('1 0 0 0 10 4.2 -12 3.1 0 0 0 0', 1021, 5.5, False, False, True, 10),
        'seed1021-bare': ('1 0 0 0 10 4.2 -12 3.1 0 0 0 0', 1021, 5.5, False, True, True, 10),
        'warm': (front, 605, 5.8, False, False, True, 17.25),
        'backlit': ('1 1.5707963 0 0 10 19.2 4 3.1 0 0 0 0', 605, 5.8, False, False, True, 17.25),
        'night': (front, 605, 5.8, False, False, True, 23),
        'group': ('', 605, 5.8, False, False, False, 10),
        'control-group': ('', 605, 5.8, True, False, False, 10),
    }
    geometry = repo / '.rae/apps/138_procgen_showcase/window.138_procgen_showcase.geometry'
    saved = geometry.read_bytes() if geometry.exists() else None
    geometry.parent.mkdir(parents=True, exist_ok=True)
    geometry.write_text('100 100 1280 800\n')
    manifest = {'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
                'binaryReused': bool(args.binary), 'sourceHashesAtCapture': {str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in [repo / 'lib/procgen/RedMaple.rae', repo / 'lib/app3d/GroveTrees.rae',
                                              repo / 'examples/138_procgen_showcase/GroveScene.rae']},
                'clock': '180 frames, 1/30 second' if args.motion else '60 frames, 1/60 second',
                'wind': args.wind, 'walkDistance': 2.5 if args.motion else 0, 'imageSize': [1280, 800],
                'assetHashes': {name: hashlib.sha256((repo / 'lib/assets/trees' / name).read_bytes()).hexdigest()
                                for name in ['redMapleSpray.png', 'redMapleBark.png']},
                'effects': 'Preetham, exposure 1, deferred toon, CSM, SSAO, SMAA, bloom',
                'position': [4.2, 4, 0], 'rotation': 0, 'captures': []}
    buildNeeded = not args.binary
    try:
        for name, (camera, seed, height, control, bare, isolated, hour) in views.items():
            if args.view != 'all' and args.view != name:
                continue
            env = dict(os.environ, RAE_UI_HEADLESS='1', RAE_HEADLESS_FRAMES='180' if args.motion else '60',
                       RAE_FIXED_DT='0.033333333' if args.motion else '0.016666667',
                       RAE_WIND=str(args.wind), RAE_DEBUG_OVERLAY='0', RAE_GROVE_VIEW='trees', RAE_CAMERA_AT=camera,
                       RAE_RED_MAPLE_CONTROL=str(int(control)), RAE_RED_MAPLE_BARE=str(int(bare)),
                       RAE_RED_MAPLE_ISOLATE=str(int(isolated)), RAE_RED_MAPLE_SEED=str(seed), RAE_RED_MAPLE_HEIGHT=str(height),
                       RAE_WALKER_HOUR=str(hour), RAE_DRS='off', RAE_RENDER_SCALE='1',
                       RAE_RUN_KEEP_BINARY=str(binary), RAE_GPU2D_SCREENSHOT=str(output / f'{name}.bmp'))
            frames = output / f'{name}-frames'
            env.update(RAE_LANDSCAPE_WALK='1' if args.motion else '',
                       RAE_LANDSCAPE_FRAMES=str(frames) if args.motion else '', RAE_LANDSCAPE_DISTANCE='2.5')
            if args.motion:
                frames.mkdir(exist_ok=True)
            command = [str(binary)]
            if buildNeeded:
                command = [str(repo / 'compiler/bin/rae'), 'run', '--project', 'examples/138_procgen_showcase',
                           'examples/138_procgen_showcase/Main.rae']
                buildNeeded = False
            with (output / f'{name}.log').open('w') as log:
                subprocess.run(command, cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
            png = output / f'{name}.png'
            subprocess.run(['sips', '-s', 'format', 'png', '-z', '800', '1280', str(frames / 'frame179.bmp' if args.motion else output / f'{name}.bmp'),
                            '--out', str(png)], check=True, timeout=20, stdout=subprocess.DEVNULL)
            if args.motion:
                subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', '30', '-i',
                                str(frames / 'frame%d.bmp'), '-vf', 'scale=1280:800', '-c:v', 'libx264',
                                '-pix_fmt', 'yuv420p', '-crf', '19', str(output / f'{name}.mp4')],
                               check=True, timeout=60)
            manifest['captures'].append({'file': png.name, 'camera': camera or 'trees preset', 'seed': seed,
                'height': height, 'width': 1.8 if isolated else None, 'control': control, 'bare': bare, 'isolated': isolated, 'hour': hour,
                'sha256': hashlib.sha256(png.read_bytes()).hexdigest()})
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
