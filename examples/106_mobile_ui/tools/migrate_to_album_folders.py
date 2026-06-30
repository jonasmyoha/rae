#!/usr/bin/env python3
"""Migrate `assets/album_art/*.jpg` + `assets/album_metadata.txt` into a
per-album folder layout:

    assets/albums/<stem>/cover.jpg
    assets/albums/<stem>/album.json

Where `<stem>` is the original filename without extension (e.g.
`mitski-puberty`). This is a one-shot script — run once, then the
`album_art/` directory and the flat `.txt` can be removed.

We deliberately do NOT re-fetch from iTunes; everything comes from the
existing `album_metadata.txt`.

`album.json` schema (all fields except `status` are optional):

    {
      "artist":     "Mitski",
      "album":      "Puberty 2",
      "year":       2016,
      "releaseDate":"2016-06-17T07:00:00Z",
      "genre":      "Indie Rock",
      "trackCount": 12,
      "artworkUrl": "https://...",
      "artistUrl":  "https://...",
      "albumUrl":   "https://...",
      "status":     "ok"
    }
"""

import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ART_DIR = os.path.join(ROOT, "assets", "album_art")
META_PATH = os.path.join(ROOT, "assets", "album_metadata.txt")
OUT_DIR = os.path.join(ROOT, "assets", "albums")

NUMERIC_KEYS = {"year", "trackCount"}
# `file` and `localPath` are layout-specific and become redundant once
# we move to a folder-per-album scheme.
DROP_KEYS = {"file", "localPath"}


def parse_records(text: str):
    for chunk in re.split(r"\n\s*\n", text):
        record = {}
        for raw in chunk.splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            key, _, value = line.partition("=")
            record[key.strip()] = value.strip()
        if record:
            yield record


def to_json(record: dict) -> dict:
    """Drop layout-only keys and coerce numeric fields to ints."""
    out = {}
    for k, v in record.items():
        if k in DROP_KEYS:
            continue
        if k in NUMERIC_KEYS:
            try:
                out[k] = int(v)
                continue
            except ValueError:
                pass
        out[k] = v
    return out


def stem_of(filename: str) -> str:
    return re.sub(r"\.(jpg|jpeg|png|webp)$", "", filename, flags=re.I)


def main():
    if not os.path.isfile(META_PATH):
        print(f"metadata file not found: {META_PATH}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isdir(ART_DIR):
        print(f"art dir not found: {ART_DIR}", file=sys.stderr)
        sys.exit(1)

    with open(META_PATH, encoding="utf-8") as f:
        text = f.read()

    os.makedirs(OUT_DIR, exist_ok=True)

    moved = 0
    skipped = 0
    missing = []

    for record in parse_records(text):
        filename = record.get("file")
        if not filename:
            continue
        stem = stem_of(filename)
        src = os.path.join(ART_DIR, filename)
        if not os.path.isfile(src):
            missing.append(filename)
            skipped += 1
            continue

        dest_dir = os.path.join(OUT_DIR, stem)
        os.makedirs(dest_dir, exist_ok=True)

        dest_cover = os.path.join(dest_dir, "cover.jpg")
        if os.path.exists(dest_cover):
            print(f"skip (cover already at dest): {stem}", file=sys.stderr)
            skipped += 1
            continue
        shutil.move(src, dest_cover)

        with open(os.path.join(dest_dir, "album.json"), "w", encoding="utf-8") as f:
            json.dump(to_json(record), f, indent=2, ensure_ascii=False)
            f.write("\n")

        moved += 1
        print(f"  {stem}", file=sys.stderr)

    print(
        f"\nMoved {moved} albums into {OUT_DIR}\n"
        f"Skipped: {skipped}",
        file=sys.stderr,
    )
    if missing:
        print(
            "Records in metadata but no cover file on disk:\n  "
            + "\n  ".join(missing),
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
