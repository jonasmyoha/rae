#!/usr/bin/env python3
"""Fetch album metadata from the iTunes Search API for every cover-art
file in `assets/album_art/`.

For each filename of the form `artist-album.jpg` (or a free-form single
name), we hit iTunes with a few progressively more-specific queries and
keep the first hit whose returned `artistName` looks like the artist we
parsed from the filename. If nothing matches we fall back to the most
relaxed search and tag the record `status=fallback`; if even that comes
up empty we emit `status=unmatched` with just the parsed strings.

Output: `assets/album_metadata.txt`, a flat record-per-album text file.

  - Records are separated by blank lines.
  - Each record is one `key=value` line.
  - Values are everything after the first `=` on a line (so they can
    contain spaces and `=`, but not literal newlines).
  - Keys: file, artist, album, year, releaseDate, genre, trackCount,
    artworkUrl, artistUrl, albumUrl, status, localPath. Not every key
    is present on every record (status=unmatched means only file,
    artist, album, status, localPath are written).

The format is deliberately trivial so the Rae-side parser is just
"split on blank lines, then split each line on the first '='."
"""

import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART_DIR = os.path.join(ROOT, "assets", "album_art")
OUT_PATH = os.path.join(ROOT, "assets", "album_metadata.txt")
ASSET_PREFIX = "examples/98_mobile_ui/assets/album_art"


# ---------- filename parsing ----------

def parse_filename(filename: str):
    """`02_mgmt-oracular_spectacular.jpg` -> ('MGMT', 'Oracular Spectacular')

    Strips any leading `NN_`/`NNN_` sort prefix, splits on the first `-`,
    and replaces `_` with spaces. If there's no `-` the whole thing is
    returned as the artist guess with an empty album guess.
    """
    stem = re.sub(r"\.(jpg|jpeg|png|webp)$", "", filename, flags=re.I)
    stem = re.sub(r"^\d+_", "", stem)

    if "-" in stem:
        a, b = stem.split("-", 1)
        return a.replace("_", " ").strip(), b.replace("_", " ").strip()
    return stem.replace("_", " ").strip(), ""


# ---------- iTunes API ----------

def itunes_search(params: dict):
    url = "https://itunes.apple.com/search?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(
        url, headers={"User-Agent": "rae-mobile-ui-fetcher/1.1"}
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data.get("results", []) or []


def normalise(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", text.lower())


def best_match(results: list, expected_artist: str):
    """First result whose artistName looks like the expected artist
    after normalisation, else None."""
    if not expected_artist:
        return results[0] if results else None
    want = normalise(expected_artist)
    for r in results:
        got = normalise(r.get("artistName", ""))
        # Either side may be a substring of the other (handles
        # "Charli xcx" vs "charlie xcx", "Bjork" vs "Björk", etc.).
        if want and got and (want in got or got in want):
            return r
    return None


def find_album(artist: str, album: str):
    """Try a few search shapes; first one to return a hit whose artist
    matches `artist` wins. Returns (hit_dict, status_str)."""
    queries = []
    if album:
        # Album-term search, narrowest. Best when both fields are known.
        queries.append((
            {
                "term": f"{artist} {album}",
                "entity": "album",
                "attribute": "albumTerm",
                "limit": "10",
            },
            "ok",
        ))
        # Fallback: just the album name as an album-term search.
        queries.append((
            {
                "term": album,
                "entity": "album",
                "attribute": "albumTerm",
                "limit": "10",
            },
            "ok",
        ))
    # Broad search across all attributes — last resort.
    queries.append((
        {
            "term": f"{artist} {album}".strip(),
            "entity": "album",
            "limit": "10",
        },
        "fallback",
    ))

    for params, ok_status in queries:
        try:
            results = itunes_search(params)
        except Exception as e:
            print(f"  ! {e}", file=sys.stderr)
            continue
        hit = best_match(results, artist)
        if hit:
            return hit, ok_status
        # Even if the strict artist check failed, hold onto the first
        # result for the fallback case below.
        first = results[0] if results else None
        if params is queries[-1][0] and first:
            # Last query, no artist-confirmed match — return first hit
            # tagged as fallback so a human can sanity-check it.
            return first, "fallback"
    return None, "unmatched"


def normalise_artwork(url: str) -> str:
    """iTunes returns a 100×100 thumb by default; swap for 600×600."""
    if not url:
        return url
    return url.replace("/100x100bb.jpg", "/600x600bb.jpg")


# ---------- record formatting ----------

def format_record(filename: str, hit, status: str, parsed_artist: str, parsed_album: str) -> str:
    lines = [f"file={filename}"]
    if hit is None:
        lines.append(f"artist={parsed_artist}")
        if parsed_album:
            lines.append(f"album={parsed_album}")
        lines.append("status=unmatched")
    else:
        lines.append(f"artist={hit.get('artistName', parsed_artist) or parsed_artist}")
        lines.append(f"album={hit.get('collectionName', parsed_album) or parsed_album}")
        rel = hit.get("releaseDate", "") or ""
        if rel:
            lines.append(f"year={rel[:4]}")
            lines.append(f"releaseDate={rel}")
        if hit.get("primaryGenreName"):
            lines.append(f"genre={hit['primaryGenreName']}")
        if hit.get("trackCount"):
            lines.append(f"trackCount={hit['trackCount']}")
        art = normalise_artwork(hit.get("artworkUrl100", ""))
        if art:
            lines.append(f"artworkUrl={art}")
        if hit.get("artistViewUrl"):
            lines.append(f"artistUrl={hit['artistViewUrl']}")
        if hit.get("collectionViewUrl"):
            lines.append(f"albumUrl={hit['collectionViewUrl']}")
        lines.append(f"status={status}")
    lines.append(f"localPath={ASSET_PREFIX}/{filename}")
    return "\n".join(lines)


# ---------- driver ----------

def main():
    if not os.path.isdir(ART_DIR):
        print(f"art dir not found: {ART_DIR}", file=sys.stderr)
        sys.exit(1)

    files = sorted(
        f for f in os.listdir(ART_DIR)
        if not f.startswith(".")
        and os.path.isfile(os.path.join(ART_DIR, f))
        and re.search(r"\.(jpg|jpeg|png|webp)$", f, flags=re.I)
    )
    print(f"Processing {len(files)} files...", file=sys.stderr)

    records = []
    counts = {"ok": 0, "fallback": 0, "unmatched": 0}
    for i, fn in enumerate(files, 1):
        artist, album = parse_filename(fn)
        print(
            f"[{i}/{len(files)}] {fn} -> artist='{artist}' album='{album}'",
            file=sys.stderr, flush=True,
        )
        hit, status = find_album(artist, album)
        counts[status] = counts.get(status, 0) + 1
        records.append(format_record(fn, hit, status, artist, album))
        time.sleep(0.4)

    header = (
        "# Album metadata fetched from the iTunes Search API.\n"
        "# Records separated by blank lines. Each record is one\n"
        "# `key=value` pair per line.\n"
        "#\n"
        "# Status values:\n"
        "#   ok        - iTunes returned a hit whose artist matches\n"
        "#               the artist parsed from the filename.\n"
        "#   fallback  - iTunes returned something but its artist\n"
        "#               didn't match; treat as a best-effort guess.\n"
        "#   unmatched - no results; only the parsed artist / album\n"
        "#               strings from the filename are present.\n"
        "#\n"
        "# Source script: examples/98_mobile_ui/tools/fetch_album_metadata.py\n"
    )
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write(header + "\n")
        f.write("\n\n".join(records))
        f.write("\n")
    print(
        f"\nWrote {len(records)} records to {OUT_PATH}\n"
        f"  ok: {counts['ok']}  fallback: {counts['fallback']}  unmatched: {counts['unmatched']}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
