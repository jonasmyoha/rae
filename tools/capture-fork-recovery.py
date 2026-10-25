#!/usr/bin/env python3
"""Capture the preserved and recovered fork studies using existing grove views.

Run from any directory; output is temporary unless an explicit folder is given.
Each app run has a 180-second timeout. No reference PNG is overwritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def capture():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default="/tmp/rae-fork-recovery-final")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    geometry = repo / ".rae/apps/138_procgen_showcase/window.138_procgen_showcase.geometry"
    saved = geometry.read_bytes() if geometry.exists() else None
    geometry.parent.mkdir(parents=True, exist_ok=True)
    geometry.write_text("100 100 1280 800\n")
    manifest = {
        "sourceState": "Working tree; sourceHashes identify the rendered implementation, including uncommitted files.",
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
        "sourceHashes": {str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in [*sorted((repo / "lib/procgen").glob("*Fork*.rae")),
                                      repo / "examples/138_procgen_showcase/ForkStudy.rae"]},
        "seeds": {"young": 206, "mature": 907},
        "transforms": {"young": {"position": [5, -14, -0.05], "rotationZ": 0.25},
                       "mature": {"position": [14, -12, -0.05], "rotationZ": -0.35}},
        "scale": 1, "cameraTarget": [9.5, -13, 6.2], "fieldOfViewDegrees": 42,
        "windowPoints": [1280, 800], "reviewPixels": [1280, 800],
        "frames": 60, "fixedDeltaTime": 0.016666667, "wind": 0,
        "sky": "Preetham", "exposure": 1,
        "effects": "deferred, cascaded shadows, SSAO, SMAA, bloom; neutral non-toon study material",
        "timingCaveat": "Generation only, two meshes, single runs with desktop background load; not a frame benchmark.",
        "captures": [],
    }
    views = {"front": (-0.10, 0.08, 27, 10), "side": (1.35, 0.08, 27, 10),
             "grazing": (-0.55, 0.02, 25, 17.25)}
    try:
        for variant in ("control", "recovery"):
            for view, camera in views.items():
                stem = f"{variant}-{view}"
                bitmap = output / f"{stem}.bmp"
                png = output / f"{stem}.png"
                env = dict(os.environ, RAE_FORK_CONTROL="1" if variant == "control" else "0",
                           RAE_GROVE_VIEW=f"forks-{view}", RAE_DEBUG_OVERLAY="0", RAE_UI_HEADLESS="1",
                           RAE_HEADLESS_FRAMES="60", RAE_FIXED_DT="0.016666667", RAE_WIND="0",
                           RAE_CAMERA_AT="", RAE_GPU2D_SCREENSHOT=str(bitmap))
                log = output / f"{stem}.log"
                with log.open("w") as stream:
                    subprocess.run(["perl", "-e", "alarm shift; exec @ARGV or die $!", "180",
                                    str(repo / "compiler/bin/rae"), "run", "--project",
                                    "examples/138_procgen_showcase", "examples/138_procgen_showcase/Main.rae"],
                                   cwd=repo, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                   check=True, timeout=190)
                subprocess.run(["sips", "-s", "format", "png", "-z", "800", "1280",
                                str(bitmap), "--out", str(png)], check=True, timeout=20,
                               stdout=subprocess.DEVNULL)
                statistics = re.search(r"\[fork-study\].*", log.read_text())
                manifest["captures"].append({"file": png.name, "variant": variant, "view": view,
                    "yaw": camera[0], "pitch": camera[1], "distance": camera[2], "hour": camera[3],
                    "statistics": statistics.group(0) if statistics else "missing",
                    "sha256": hashlib.sha256(png.read_bytes()).hexdigest()})
                (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
                print(f"Captured {stem}", flush=True)
    finally:
        if saved is None:
            geometry.unlink(missing_ok=True)
        else:
            geometry.write_bytes(saved)


if __name__ == "__main__":
    capture()
