#!/usr/bin/env python3
"""Enrich every `assets/albums/<stem>/album.json` with track data.

For each album.json with an `albumUrl` of the form
`https://music.apple.com/.../<collectionId>?uo=4`, we hit the iTunes
`/lookup` endpoint to fetch the album's track list and write back:

    tracks: [
      { "number": 1, "title": "Happy", "durationMs": 220000 },
      ...
    ]
    albumDurationMs: 1620000        # sum of trackTimeMillis

If the album.json has no albumUrl (status=unmatched), or the lookup
returns no track rows, we write `tracks: []` and `albumDurationMs: 0`.

Re-runnable. Existing track data is overwritten so a re-run picks up
upstream corrections.
"""

import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

ALBUMS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "assets",
    "albums",
)

LOOKUP = "https://itunes.apple.com/lookup"


def collection_id_from_url(url: str) -> str | None:
    if not url:
        return None
    # `.../puberty-2/1087128495?uo=4` — grab the trailing digit run.
    m = re.search(r"/(\d{6,})\??", url)
    return m.group(1) if m else None


def lookup_tracks(collection_id: str):
    params = {"id": collection_id, "entity": "song", "limit": "200"}
    url = LOOKUP + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(
        url, headers={"User-Agent": "rae-mobile-ui-fetcher/1.2"}
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    tracks = []
    for r in data.get("results", []) or []:
        if r.get("wrapperType") != "track":
            continue
        ms = r.get("trackTimeMillis") or 0
        tracks.append({
            "number": r.get("trackNumber") or 0,
            "title": r.get("trackName") or "",
            "durationMs": int(ms),
        })
    tracks.sort(key=lambda t: t["number"])
    return tracks


def main():
    if not os.path.isdir(ALBUMS_DIR):
        print(f"albums dir not found: {ALBUMS_DIR}", file=sys.stderr)
        sys.exit(1)

    stems = sorted(
        d for d in os.listdir(ALBUMS_DIR)
        if os.path.isdir(os.path.join(ALBUMS_DIR, d))
    )
    print(f"Enriching {len(stems)} albums...", file=sys.stderr)

    counts = {"ok": 0, "no_url": 0, "no_tracks": 0}
    for i, stem in enumerate(stems, 1):
        path = os.path.join(ALBUMS_DIR, stem, "album.json")
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            data = json.load(f)

        cid = collection_id_from_url(data.get("albumUrl", ""))
        if not cid:
            data["tracks"] = []
            data["albumDurationMs"] = 0
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
                f.write("\n")
            counts["no_url"] += 1
            print(f"[{i}/{len(stems)}] {stem}: no albumUrl", file=sys.stderr)
            continue

        try:
            tracks = lookup_tracks(cid)
        except Exception as e:
            print(f"[{i}/{len(stems)}] {stem}: lookup failed — {e}", file=sys.stderr)
            tracks = []

        total_ms = sum(t["durationMs"] for t in tracks)
        data["tracks"] = tracks
        data["albumDurationMs"] = total_ms

        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
            f.write("\n")

        if tracks:
            secs = total_ms // 1000
            print(
                f"[{i}/{len(stems)}] {stem}: {len(tracks)} tracks, "
                f"{secs // 60}:{secs % 60:02d}",
                file=sys.stderr,
            )
            counts["ok"] += 1
        else:
            counts["no_tracks"] += 1
            print(f"[{i}/{len(stems)}] {stem}: no tracks returned", file=sys.stderr)

        time.sleep(0.4)

    print(
        f"\nDone. ok: {counts['ok']}  no_url: {counts['no_url']}  "
        f"no_tracks: {counts['no_tracks']}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
