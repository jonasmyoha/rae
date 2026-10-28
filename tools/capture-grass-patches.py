#!/usr/bin/env python3
"""Matched grass patch evidence. Full frames, clips and logs stay outside git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--before', type=Path, required=True, help='folder with preserved before138/before143 binaries')
    parser.add_argument('--motion', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = {'before': '9774d0de plus the same optional CameraStudy capture hook',
                'densityScale': 0.55, 'fixedDelta': 1 / 30, 'sunHour': 10, 'dynamicResolution': False, 'renderScale': 1.0,
                'heightScale': 0.55, 'widthScale': 1.8,
                'instances': 'same density budget, seed and exclusions; regrouping changes surviving count',
                'sources': {}, 'captures': [], 'clips': []}
    for name in ['lib/grass_field.wgsl', 'lib/grass_compute.wgsl', 'lib/grass_motion.wgsl',
                 'lib/grass_render.wgsl', 'lib/GrassConfig.rae', 'lib/terrain_grass_painted.wgsl',
                 'lib/app3d/CameraStudy.rae', 'examples/138_procgen_showcase/assets/grove_ground.wgsl']:
        manifest['sources'][name] = hashlib.sha256((repo / name).read_bytes()).hexdigest()

    def save():
        manifest['binaryHashes'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                    for path in [args.before.resolve()/'before138', args.before.resolve()/'before143',
                                                 output/'after138', output/'after143'] if path.exists()}
        (output / ('motion.json' if args.motion else 'manifest.json')).write_text(json.dumps(manifest, indent=2) + '\n')

    def run(app, style, label, camera, wind=0, motion=False, hour=10):
        number = app[:3]
        name = f'{number}-{style}-{label}'
        binary = args.before.resolve() / f'before{number}' if style == 'before' else output / f'after{number}'
        geometry = repo / f'.rae/apps/{app}/window.{app}.geometry'
        saved = geometry.read_bytes() if geometry.exists() else None
        geometry.parent.mkdir(parents=True, exist_ok=True)
        geometry.write_text('100 100 640 400\n' if motion else '100 100 1280 800\n')
        frames = output / name
        if motion:
            frames.mkdir(exist_ok=True)
        bitmap = output / f'{name}.bmp'
        env = dict(os.environ, RAE_UI_HEADLESS='1', RAE_FIXED_DT='0.033333333',
                   RAE_HEADLESS_FRAMES='180' if motion else '60', RAE_WIND=str(wind),
                   RAE_GROVE_VIEW='grass', RAE_WALKER_HOUR=str(hour), RAE_CAMERA_AT=camera,
                   RAE_DEBUG_OVERLAY='0', RAE_DRS='off', RAE_RENDER_SCALE='1', RAE_GRASS_REPORT='1', RAE_LANDSCAPE_WALK='1' if motion else '',
                   RAE_LANDSCAPE_FRAMES=str(frames) if motion else '', RAE_GPU2D_SCREENSHOT=str(bitmap),
                   RAE_RUN_KEEP_BINARY=str(binary))
        command = [str(binary)] if binary.exists() else [str(repo/'compiler/bin/rae'), 'run', '--project',
                                                         f'examples/{app}', f'examples/{app}/Main.rae']
        try:
            with (output/f'{name}.log').open('w') as log:
                subprocess.run(['perl', '-e', 'alarm shift; exec @ARGV', '180'] + command,
                               cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=190)
            if motion:
                assert len(list(frames.glob('frame*.bmp'))) == 180, name
                clip = output / f'{name}.mp4'
                subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', '30', '-i',
                                str(frames/'frame%d.bmp'), '-c:v', 'libx264', '-crf', '18', '-pix_fmt',
                                'yuv420p', str(clip)], check=True, timeout=60)
                manifest['clips'].append({'file': clip.name, 'camera': camera, 'wind': wind,
                                          'frames': 180, 'pathMeters': 12, 'eye': 'fixed world height',
                                          'bitmapPixels': list(struct.unpack_from('<ii', (frames/'frame0.bmp').read_bytes(), 18)),
                                          'sha256': hashlib.sha256(clip.read_bytes()).hexdigest()})
            else:
                png = output / f'{name}.png'
                subprocess.run(['sips', '-s', 'format', 'png', '-z', '800', '1280', str(bitmap),
                                '--out', str(png)], check=True, timeout=20, stdout=subprocess.DEVNULL)
                manifest['captures'].append({'file': png.name, 'camera': camera, 'wind': wind, 'hour': hour,
                                             'bitmapPixels': list(struct.unpack_from('<ii', bitmap.read_bytes(), 18)),
                                             'sha256': hashlib.sha256(png.read_bytes()).hexdigest()})
            save()
            print(name, flush=True)
        finally:
            if saved is None:
                geometry.unlink(missing_ok=True)
            else:
                geometry.write_bytes(saved)

    views = {
        '138_procgen_showcase': {
            'clearing': '1 0 0.24 -0.28 4.2 -8 -6 0.8 0 0 0 0',
            'edge': '1 -0.2 0.24 -0.22 4.2 -13 -1 1.7 0 0 0 0',
            'path': '1 0.05 0.24 -0.26 4.2 -2 -11 1.0 0 0 0 0'},
        '143_walker_forest': {
            'clearing': '1 0 0.24 -0.24 4.2 -4 -5 1.7 0 0 0 0',
            'edge': '1 -0.4 0.24 -0.22 4.2 -9 2 1.7 0 0 0 0',
            'path': '1 0.3 0.24 -0.22 4.2 0 -15 1.7 0 0 0 0'}}
    if not args.motion:
        for number in ['138', '143']:
            (output/f'after{number}').unlink(missing_ok=True)
        for app, scenes in views.items():
            for label, camera in scenes.items():
                for style in ['before', 'after']:
                    run(app, style, label, camera)
            run(app, 'after', 'night', scenes['clearing'], hour=23)
            run(app, 'after', 'repeat', scenes['clearing'])
            assert (output/f'{app[:3]}-after-repeat.bmp').read_bytes() == (output/f'{app[:3]}-after-clearing.bmp').read_bytes()
        manifest['repeatBitmapsIdentical'] = True
        save()
    else:
        for app in views:
            camera = ('1 0 0.24 -0.24 4.2 -8 -8 1.7 0 0 0 0' if app.startswith('138')
                      else views[app]['clearing'])
            for style in ['before', 'after']:
                for label, wind in [('calm', 0), ('breeze', 1.5)]:
                    run(app, style, label, camera, wind=wind, motion=True)
        (output/'motion.json').write_text(json.dumps(manifest, indent=2)+'\n')


if __name__ == '__main__':
    main()
