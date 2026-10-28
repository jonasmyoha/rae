#!/usr/bin/env python3
"""Controlled crown studies; full bitmaps and continuous clips stay outside git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", default="/tmp/rae-birch-recovery-final")
    parser.add_argument("--reuse-binary", action="store_true")
    parser.add_argument("--motion", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "app"
    geometry = repo / ".rae/apps/138_procgen_showcase/window.138_procgen_showcase.geometry"
    saved = geometry.read_bytes() if geometry.exists() else None
    geometry.parent.mkdir(parents=True, exist_ok=True)
    geometry.write_text("100 100 1280 800\n")
    base = dict(os.environ, RAE_BIRCH_STUDY="tree", RAE_BIRCH_ISOLATE="1", RAE_TREELAB_STUDY="birches",
                RAE_UI_HEADLESS="1", RAE_HEADLESS_FRAMES="60", RAE_FIXED_DT="0.016666667",
                RAE_DEBUG_OVERLAY="0", RAE_WIND="0", RAE_BIRCH_MOTION="", RAE_BIRCH_FRAMES="")
    manifest = {"seed": 1437, "height": 7.1, "position": [-16, -10, 0], "rotation": 0,
                "branchIndex": 8, "treeCameraTargetZ": 4.3, "isolated": True, "wood": "unchanged newer scaffold for all alternatives",
                "windowPoints": [1280, 800], "reviewPixels": [1280, 800],
                "frames": 60, "fixedDelta": 0.016666667, "wind": 0,
                "sky": "Preetham", "exposure": 1, "sunStrength": 3.1,
                "effects": "cascaded shadows, SSAO, SMAA, bloom; ordinary lit materials",
                "sources": {}, "captures": []}
    for name in ("lib/procgen/Birch.rae", "lib/procgen/FoliageCards.rae",
                 "lib/assets/trees/birchLeaves.png", "lib/assets/trees/birchSpray.png",
                 "examples/146_treeLab/BirchGrove.rae",
                 "examples/146_treeLab/BirchStudy.rae",
                 "examples/146_treeLab/Main.rae",
                 "examples/146_treeLab/StudyTrees.rae",
                 "lib/stylizedTrees/StylizedTrees.rae"):
        manifest["sources"][name] = hashlib.sha256((repo / name).read_bytes()).hexdigest()

    def run(name, env, command=None):
        with (output / f"{name}.log").open("w") as log:
            subprocess.run(["perl", "-e", "alarm shift; exec @ARGV or die $!", "180"] +
                           (command or [str(binary)]), cwd=repo, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=190)

    def capture(style, view, camera, study="tree", hour=10, isolate=True):
        name = f"{style}-{study}-{view}"
        bitmap = output / f"{name}.bmp"
        png = output / f"{name}.png"
        preset = "birches" if view in ("front", "side", "branch", "repeat", "birches") else f"birches-{view}"
        env = dict(base, RAE_BIRCH_STYLE=style, RAE_BIRCH_STUDY="" if study == "grove" else study,
                   RAE_BIRCH_ISOLATE="1" if isolate else "0",
                   RAE_TREELAB_STUDY=preset, RAE_CAMERA_AT=camera, RAE_GPU2D_SCREENSHOT=str(bitmap))
        run(name, env)
        subprocess.run(["sips", "-s", "format", "png", "-z", "800", "1280",
                        str(bitmap), "--out", str(png)], check=True, timeout=20,
                       stdout=subprocess.DEVNULL)
        manifest["captures"].append({"file": png.name, "style": style, "study": study,
                                     "view": view, "camera": camera, "hour": hour, "isolated": isolate,
                                     "sha256": hashlib.sha256(png.read_bytes()).hexdigest()})
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(name, flush=True)

    front = "0 0.1 0.12 0 16 0 0 0 -16 -10 0 0"
    branch = "1 0 0 0 5 -15.2241 -13.60058 6.59539 0 0 0 0"
    try:
        if not args.reuse_binary:
            run("build", dict(base, RAE_HEADLESS_FRAMES="1", RAE_RUN_KEEP_BINARY=str(binary)),
                [str(repo / "compiler/bin/rae"), "run", "--project",
                 "examples/146_treeLab", "examples/146_treeLab/Main.rae"])
        for style in ("control", "texture", "connected"):
            capture(style, "front", front)
            capture(style, "branch", branch, study="branch")
        for view, camera, hour in (
                ("side", "0 1.6 0.12 0 16 0 0 0 -16 -10 0 0", 10),
                ("under", "0 0.38 -0.35 0 9 0 0 0 -16 -10 0 0", 10),
                ("warm", front, 17.25), ("cool", front, 7.25), ("night", front, 23)):
            capture("connected", view, camera, hour=hour)
        capture("connected", "repeat", front)
        if (output / "connected-tree-front.bmp").read_bytes() != (output / "connected-tree-repeat.bmp").read_bytes():
            raise RuntimeError("fixed captures differ")
        for style in ("control", "connected"):
            capture(style, "birches", "", study="grove", isolate=False)
        if args.motion:
            manifest["motion"] = {"frames": 180, "fixedDelta": 0.033333333, "wind": 1,
                                  "styles": ["control", "connected"], "paths": ["orbit", "walk"],
                                  "walkEyeHeight": 1.7, "windowPoints": [640, 400]}
            manifest["clips"] = []
            geometry.write_text("100 100 640 400\n")
            for style in ("control", "connected"):
                for motion in ("orbit", "walk"):
                    name = motion if style == "connected" else f"control-{motion}"
                    frames = output / name
                    frames.mkdir(exist_ok=True)
                    run(name, dict(base, RAE_BIRCH_STYLE=style, RAE_BIRCH_MOTION=motion,
                                   RAE_BIRCH_FRAMES=str(frames), RAE_HEADLESS_FRAMES="180",
                                   RAE_FIXED_DT="0.033333333", RAE_WIND="1", RAE_CAMERA_AT=front))
                    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30",
                                    "-i", str(frames / "frame%d.bmp"), "-c:v", "libx264",
                                    "-crf", "18", "-pix_fmt", "yuv420p", str(output / f"{name}.mp4")],
                                   check=True, timeout=60)
                    clip = output / f"{name}.mp4"
                    manifest["clips"].append({"file": clip.name, "sha256": hashlib.sha256(clip.read_bytes()).hexdigest()})
                    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
                    print(f"continuous {name}: 180 frames, 6 seconds, wind 1", flush=True)
    finally:
        if saved is None:
            geometry.unlink(missing_ok=True)
        else:
            geometry.write_bytes(saved)


if __name__ == "__main__":
    main()
