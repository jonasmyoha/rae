#!/usr/bin/env python3
"""Render matched near-grass alternatives; keep full bitmaps and logs outside git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default="/tmp/rae-grass-comparison")
    parser.add_argument("--baseline", type=Path, help="optional extracted e86992db tree")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"frames": 60, "fixedDelta": 0.016666667, "wind": 0,
                "windowPoints": [1280, 800], "reviewPixels": [1280, 800],
                "heightScale": 0.55, "widthScale": 1.8, "densityScale": 0.55,
                "placement": "unchanged shared compute; identical within each app",
                "sources": {}, "captures": []}
    for name in ("lib/GrassConfig.rae", "lib/GrassCompute.rae", "lib/grass_render.wgsl",
                 "lib/grass_compute.wgsl", "lib/assets/grass/meadowTuft.png",
                 "examples/138_procgen_showcase/GroveGrass.rae", "lib/app3d/WalkerRender.rae"):
        manifest["sources"][name] = hashlib.sha256((repo / name).read_bytes()).hexdigest()

    def capture(app, shape, view, camera, hour=10, root=repo):
        historical = root != repo
        name = "pre-queue-close" if historical else f"{app}-{shape}-{view}"
        binary = output / ("baseline-app" if historical else f"{app}-app")
        geometry = root / f".rae/apps/{app}/window.{app}.geometry"
        saved = geometry.read_bytes() if geometry.exists() else None
        geometry.parent.mkdir(parents=True, exist_ok=True)
        geometry.write_text("100 100 1280 800\n")
        bitmap, png = output / f"{name}.bmp", output / f"{name}.png"
        env = dict(os.environ, RAE_STDLIB=str(root / "lib"), RAE_GRASS_SHAPE=shape,
                   RAE_GRASS_REPORT="1", RAE_UI_HEADLESS="1", RAE_HEADLESS_FRAMES="60",
                   RAE_FIXED_DT="0.016666667", RAE_WIND="0", RAE_DEBUG_OVERLAY="0",
                   RAE_GROVE_VIEW="grass", RAE_CAMERA_AT=camera, RAE_WALKER_HOUR=str(hour),
                   RAE_GPU2D_SCREENSHOT=str(bitmap), RAE_RUN_KEEP_BINARY=str(binary))
        command = [str(binary)] if binary.exists() else [str(repo / "compiler/bin/rae"), "run",
                  "--project", f"examples/{app}", f"examples/{app}/Main.rae"]
        try:
            with (output / f"{name}.log").open("w") as log:
                subprocess.run(["perl", "-e", "alarm shift; exec @ARGV", "180"] + command,
                               cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=190)
            subprocess.run(["sips", "-s", "format", "png", "-z", "800", "1280", str(bitmap),
                            "--out", str(png)], check=True, timeout=20, stdout=subprocess.DEVNULL)
            manifest["captures"].append({"file": png.name, "app": app, "shape": shape,
                                         "camera": camera, "hour": hour, "preset": "grass",
                                         "historical": historical,
                                         "bitmapPixels": list(struct.unpack_from("<ii", bitmap.read_bytes(), 18)),
                                         "sha256": hashlib.sha256(png.read_bytes()).hexdigest()})
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            print(name, flush=True)
        finally:
            if saved is None:
                geometry.unlink(missing_ok=True)
            else:
                geometry.write_bytes(saved)

    # Always rebuild for this run, even when the output folder already exists.
    for name in ("138_procgen_showcase-app", "143_walker_forest-app", "baseline-app"):
        (output / name).unlink(missing_ok=True)
    close = "1 0 0.24 -0.28 4.2 -8 -6 0.8 0 0 0 0"
    eye = "1 0 0.24 -0.24 4.2 -4 -5 1.7 0 0 0 0"
    if args.baseline:
        capture("138_procgen_showcase", "historical", "close", close, root=args.baseline.resolve())
    for app, view, camera in (("138_procgen_showcase", "close", close),
                              ("143_walker_forest", "eye", eye)):
        for shape in ("control", "rejected", "ribbons", "textured"):
            capture(app, shape, view, camera)
        for light, hour in (("warm", 17.25), ("night", 23)):
            capture(app, "textured", light, camera, hour)
        capture(app, "textured", "repeat", camera)
        if (output / f"{app}-textured-{view}.bmp").read_bytes() != (output / f"{app}-textured-repeat.bmp").read_bytes():
            raise RuntimeError(f"fixed captures differ in {app}")

    manifest["repeatBitmapsIdentical"] = ["138_procgen_showcase", "143_walker_forest"]
    capture("138_procgen_showcase", "textured", "shadow", "")


if __name__ == "__main__":
    main()
