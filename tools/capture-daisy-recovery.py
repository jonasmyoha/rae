#!/usr/bin/env python3
"""Matched daisy controls and candidates; full frames and logs stay outside git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--forest', action='store_true')
    parser.add_argument('--view', help='capture just one named view')
    parser.add_argument('--reuse-binary', action='store_true')
    parser.add_argument('--wind', type=float, default=0)
    parser.add_argument('--frames', type=int, default=60)
    parser.add_argument('--tag', default='', help='suffix for phase studies')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    app = '143_walker_forest' if args.forest else '138_procgen_showcase'
    binary = output / app
    if not args.reuse_binary:
        binary.unlink(missing_ok=True)
    views = [('pair', '1 0 0.24 -0.20 4.2 -3.3 -15.7 0.85 0 0 0 0', 10),
             ('head', '1 0 0.24 -0.57 4.2 -2.965 -14.4 0.82 0 0 0 0', 10),
             ('side', '1 1.5708 0.24 -0.12 4.2 -2.3 -13.92 0.59 0 0 0 0', 10),
             ('backlit', '1 3.14159 0.24 -0.28 4.2 -2.965 -13.25 0.73 0 0 0 0', 16),
             ('patch', '1 0 0.24 -0.30 4.2 -0.8 -16.5 1.0 0 0 0 0', 10),
             ('patch-backlit', '1 3.14159 0.24 -0.28 4.2 -0.8 -11.5 1.0 0 0 0 0', 16),
             ('night', '1 0 0.24 -0.30 4.2 -0.8 -16.5 1.0 0 0 0 0', 23),
             ('single-night', '1 0 0.24 -0.20 4.2 -3.3 -15.7 0.85 0 0 0 0', 23)]
    if args.forest:
        views = [('eye', os.environ['DAISY_FOREST_CAMERA'], 10),
                 ('night', os.environ['DAISY_FOREST_CAMERA'], 23)]
    if args.view:
        views = [view for view in views if view[0] == args.view]
        assert views, args.view
    sources = ['lib/procgen/MeadowFlower.rae', 'lib/procgen/OxeyeGeometry.rae',
               'lib/app3d/WalkerFlowers.rae', 'lib/app3d/WalkerTerrain.rae',
               'examples/138_procgen_showcase/MeadowFlowers.rae',
               'examples/138_procgen_showcase/GroveGrass.rae',
               'examples/138_procgen_showcase/GroveScene.rae']
    manifest = {'app': app, 'fixedDelta': 1 / 30, 'frames': args.frames, 'wind': args.wind,
                'seeds': {'single': 1307, 'showcasePatch': 1871, 'forestPatch': 584},
                'control': 'unchanged old generator and materials, same seed/height law and world placement',
                'effects': 'existing deferred shadows/SSAO/SMAA/bloom; DRS off, scale 1, exposure 1',
                'sources': {name: hashlib.sha256((repo/name).read_bytes()).hexdigest() for name in sources},
                'captures': []}
    geometry = repo/f'.rae/apps/{app}/window.{app}.geometry'
    saved = geometry.read_bytes() if geometry.exists() else None
    geometry.parent.mkdir(parents=True, exist_ok=True)
    geometry.write_text('100 100 1280 800\n')
    try:
        for label, camera, hour in views:
            for style in ['control', 'candidate']:
                name = f'{app[:3]}-{style}-{label}{args.tag}'
                bitmap = output/f'{name}.bmp'
                env = dict(os.environ, RAE_UI_HEADLESS='1', RAE_HEADLESS_FRAMES=str(args.frames),
                           RAE_FIXED_DT='0.033333333', RAE_WIND=str(args.wind), RAE_GROVE_VIEW='flowers',
                           RAE_WALKER_HOUR=str(hour), RAE_CAMERA_AT=camera, RAE_DEBUG_OVERLAY='0',
                           RAE_DRS='off', RAE_RENDER_SCALE='1', RAE_GPU2D_SCREENSHOT=str(bitmap),
                           RAE_DAISY_CONTROL='1' if style == 'control' else '0',
                           RAE_RUN_KEEP_BINARY=str(binary))
                command = [str(binary)] if binary.exists() else [str(repo/'compiler/bin/rae'), 'run',
                           '--project', f'examples/{app}', f'examples/{app}/Main.rae']
                with (output/f'{name}.log').open('w') as log:
                    subprocess.run(['perl', '-e', 'alarm shift; exec @ARGV', '180'] + command,
                                   env=env, cwd=repo, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=190)
                png = output/f'{name}.png'
                subprocess.run(['sips', '-s', 'format', 'png', '-z', '800', '1280', str(bitmap),
                                '--out', str(png)], check=True, timeout=20, stdout=subprocess.DEVNULL)
                manifest['captures'].append({'file': png.name, 'camera': camera, 'hour': hour,
                                             'sha256': hashlib.sha256(png.read_bytes()).hexdigest()})
                print(name, flush=True)
        manifest['binaryHash'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        (output/f'{app[:3]}-{args.view or 'manifest'}{args.tag}.json').write_text(json.dumps(manifest, indent=2)+'\n')
    finally:
        if saved is None:
            geometry.unlink(missing_ok=True)
        else:
            geometry.write_bytes(saved)


if __name__ == '__main__':
    main()
