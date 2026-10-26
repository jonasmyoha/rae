#!/usr/bin/env python3
"""Matched flower controls and candidates; full frames and logs stay outside git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--forest', action='store_true')
    parser.add_argument('--species', choices=['daisy', 'harebell'], default='daisy')
    parser.add_argument('--view', help='capture just one named view')
    parser.add_argument('--reuse-binary', action='store_true')
    parser.add_argument('--wind', type=float, default=0)
    parser.add_argument('--frames', type=int, default=60)
    parser.add_argument('--motion', action='store_true', help='record every fixed-step frame as an MP4')
    parser.add_argument('--travel', type=float, default=0, help='motion camera travel in metres, 0 for stationary wind')
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
    if args.species == 'harebell':
        views = [('pair', '1 0 0.24 -0.16 4.2 -5.5 -15.45 0.57 0 0 0 0', 10),
                 ('head', '1 0 0.24 -0.08 4.2 -5.14 -14.48 0.51 0 0 0 0', 10),
                 ('side', '1 1.5708 0.24 -0.12 4.2 -4.5 -13.96 0.50 0 0 0 0', 10),
                 ('backlit', '1 3.14159 0.24 -0.16 4.2 -5.14 -13.35 0.54 0 0 0 0', 16),
                 ('backlight', '1 1.83 0.24 -0.12 4.2 -4.45 -13.8 0.50 0 0 0 0', 17),
                 ('patch', '1 3.14159 0.24 -0.30 4.2 -7.2 -11.6 0.95 0 0 0 0', 10),
                 ('eye', '1 3.14159 0.24 -0.40 4.2 -7.2 -10.8 1.65 0 0 0 0', 10),
                 ('patch-backlit', '1 3.14159 0.24 -0.22 4.2 -7.2 -11.5 0.95 0 0 0 0', 16),
                 ('night', '1 3.14159 0.24 -0.30 4.2 -7.2 -11.6 0.95 0 0 0 0', 23),
                 ('single-night', '1 0 0.24 -0.16 4.2 -5.5 -15.45 0.57 0 0 0 0', 23)]
    if args.forest:
        views = [('eye', os.environ.get('FLOWER_FOREST_CAMERA', os.environ.get('DAISY_FOREST_CAMERA', '1 1.5708 0.24 -0.38 4.2 -15.4 5.4 1.745 0 0 0 0')), 10),
                 ('night', os.environ.get('FLOWER_FOREST_CAMERA', os.environ.get('DAISY_FOREST_CAMERA', '1 1.5708 0.24 -0.38 4.2 -15.4 5.4 1.745 0 0 0 0')), 23)]
    if args.view:
        views = [view for view in views if view[0] == args.view]
        assert views, args.view
    sources = ['lib/procgen/MeadowFlower.rae', 'lib/procgen/OxeyeGeometry.rae',
               'lib/app3d/WalkerFlowers.rae', 'lib/app3d/WalkerTerrain.rae',
               'examples/138_procgen_showcase/MeadowFlowers.rae',
               'examples/138_procgen_showcase/GroveGrass.rae',
               'examples/138_procgen_showcase/GroveScene.rae', 'lib/app3d/CameraStudy.rae']
    if args.species == 'harebell':
        sources += ['lib/procgen/Harebell.rae', 'lib/procgen/HarebellGeometry.rae']
    manifest = {'species': args.species, 'app': app, 'fixedDelta': 1 / 30, 'motion': args.motion, 'travelMetres': args.travel, 'frames': args.frames, 'wind': args.wind,
                'seeds': ({'single': 1307, 'showcasePatch': 1871, 'forestPatch': 584} if args.species == 'daisy' else {'single': 2203, 'showcasePatch': 2371, 'forestPatch': 941}),
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
                           **{'RAE_DAISY_CONTROL' if args.species == 'daisy' else 'RAE_HAREBELL_CONTROL': '1' if style == 'control' else '0'},
                           RAE_RUN_KEEP_BINARY=str(binary))
                if args.motion:
                    frames = output / (name + '-frames')
                    if frames.exists():
                        shutil.rmtree(frames)
                    frames.mkdir()
                    env.update(RAE_LANDSCAPE_WALK='1', RAE_LANDSCAPE_FRAMES=str(frames),
                               RAE_LANDSCAPE_DISTANCE=str(args.travel))
                else:
                    env.update(RAE_LANDSCAPE_WALK='', RAE_LANDSCAPE_FRAMES='')
                command = [str(binary)] if binary.exists() else [str(repo/'compiler/bin/rae'), 'run',
                           '--project', f'examples/{app}', f'examples/{app}/Main.rae']
                with (output/f'{name}.log').open('w') as log:
                    subprocess.run(['perl', '-e', 'alarm shift; exec @ARGV', '180'] + command,
                                   env=env, cwd=repo, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=190)
                if args.motion:
                    actual = len(list(frames.glob('frame*.bmp')))
                    assert actual == args.frames, (actual, args.frames)
                    bitmap = frames / f'frame{args.frames - 1}.bmp'
                    movie = output / (name + '.mp4')
                    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', '30',
                                    '-i', str(frames / 'frame%d.bmp'), '-vf', 'scale=1280:800',
                                    '-c:v', 'libx264', '-preset', 'fast', '-crf', '20',
                                    '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(movie)],
                                   check=True, timeout=120)
                    manifest['captures'].append({'movie': movie.name, 'frames': actual,
                                                'sha256': hashlib.sha256(movie.read_bytes()).hexdigest()})
                png = output/f'{name}.png'
                subprocess.run(['sips', '-s', 'format', 'png', '-z', '800', '1280', str(bitmap),
                                '--out', str(png)], check=True, timeout=20, stdout=subprocess.DEVNULL)
                manifest['captures'].append({'file': png.name, 'camera': camera, 'hour': hour,
                                             'sha256': hashlib.sha256(png.read_bytes()).hexdigest()})
                if args.motion:
                    shutil.rmtree(frames)
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
