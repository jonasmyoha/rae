#!/usr/bin/env python3
"""Matched maple studies in the real grove. Large output stays outside git.

Uses existing free-camera controls. Optional isolation removes occluders but
keeps the first grove specimen at (-4.8, 2, 0). Every child has a timeout.
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
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = Path(args.binary).resolve() if args.binary else output / 'grove'
    front = '1 0 0 0 10 -4.8 -11 2.6 0 0 0 0'
    side = '1 -1.5707963 0 0 10 -17.8 2 2.6 0 0 0 0'
    under = '1 0 0 0.47 10 -4.8 -4 1.2 0 0 0 0'
    views = {
        'control-front': (front, 501, 4.7, True, False, True, 10),
        'control-bare': (front, 501, 4.7, True, True, True, 10),
        'front': (front, 501, 4.7, False, False, True, 10),
        'side': (side, 501, 4.7, False, False, True, 10),
        'under': (under, 501, 4.7, False, False, True, 10),
        'bare-front': (front, 501, 4.7, False, True, True, 10),
        'bare-side': (side, 501, 4.7, False, True, True, 10),
        'seed710': (front, 710, 6.4, False, False, True, 10),
        'seed710-bare': (front, 710, 6.4, False, True, True, 10),
        'seed917': ('1 0 0 0 10 -4.8 -14 3.6 0 0 0 0', 917, 5.5, False, False, True, 10),
        'seed917-bare': ('1 0 0 0 10 -4.8 -14 3.6 0 0 0 0', 917, 5.5, False, True, True, 10),
        'warm': (front, 501, 4.7, False, False, True, 17.25),
        'backlit': ('1 1.5707963 0 0 10 8.2 2 2.6 0 0 0 0', 501, 4.7, False, False, True, 17.25),
        'night': (front, 501, 4.7, False, False, True, 23),
        'group': ('', 501, 4.7, False, False, False, 10),
        'control-group': ('', 501, 4.7, True, False, False, 10),
    }
    geometry = repo / '.rae/apps/138_procgen_showcase/window.138_procgen_showcase.geometry'
    saved = geometry.read_bytes() if geometry.exists() else None
    geometry.parent.mkdir(parents=True, exist_ok=True)
    geometry.write_text('100 100 1280 800\n')
    manifest = {'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
                'sourceHashes': {str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in [repo / 'lib/procgen/SugarMaple.rae', repo / 'lib/app3d/GroveTrees.rae',
                                              repo / 'examples/138_procgen_showcase/GroveScene.rae']},
                'clock': '60 frames, 1/60 second, wind 0', 'imageSize': [1280, 800],
                'assetHashes': {name: hashlib.sha256((repo / 'lib/assets/trees' / name).read_bytes()).hexdigest()
                                for name in ['sugarMapleSpray.png', 'sugarMapleBark.png']},
                'effects': 'Preetham, exposure 1, deferred toon, CSM, SSAO, SMAA, bloom',
                'position': [-4.8, 2, 0], 'rotation': 0, 'captures': []}
    buildNeeded = not args.binary
    try:
        for name, (camera, seed, height, control, bare, isolated, hour) in views.items():
            if args.view != 'all' and args.view != name:
                continue
            env = dict(os.environ, RAE_UI_HEADLESS='1', RAE_HEADLESS_FRAMES='60', RAE_FIXED_DT='0.016666667',
                       RAE_WIND='0', RAE_DEBUG_OVERLAY='0', RAE_GROVE_VIEW='trees', RAE_CAMERA_AT=camera,
                       RAE_MAPLE_CONTROL=str(int(control)), RAE_MAPLE_BARE=str(int(bare)),
                       RAE_MAPLE_ISOLATE=str(int(isolated)), RAE_MAPLE_SEED=str(seed), RAE_MAPLE_HEIGHT=str(height),
                       RAE_WALKER_HOUR=str(hour), RAE_DRS='off', RAE_RENDER_SCALE='1',
                       RAE_RUN_KEEP_BINARY=str(binary), RAE_GPU2D_SCREENSHOT=str(output / f'{name}.bmp'))
            command = [str(binary)]
            if buildNeeded:
                command = [str(repo / 'compiler/bin/rae'), 'run', '--project', 'examples/138_procgen_showcase',
                           'examples/138_procgen_showcase/Main.rae']
                buildNeeded = False
            with (output / f'{name}.log').open('w') as log:
                subprocess.run(command, cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
            png = output / f'{name}.png'
            subprocess.run(['sips', '-s', 'format', 'png', '-z', '800', '1280', str(output / f'{name}.bmp'),
                            '--out', str(png)], check=True, timeout=20, stdout=subprocess.DEVNULL)
            manifest['captures'].append({'file': png.name, 'camera': camera or 'trees preset', 'seed': seed,
                'height': height, 'width': 2.05 if isolated else None, 'control': control, 'bare': bare, 'isolated': isolated, 'hour': hour,
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
