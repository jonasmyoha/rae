#!/usr/bin/env python3
"""Re-enrich every `assets/albums/<stem>/album.json` with tracklist
data from MusicBrainz — the open music database with much better
coverage than iTunes for non-mainstream / non-US releases.

Strategy:
  1. Parse the folder STEM (artist-album form) for the ground-truth
     artist + album. The iTunes-derived fields in album.json may
     be wrong (Pablo Honey was matched to "boy pablo", BRAT to a
     single, Currents to a remix EP) — don't trust them, use the
     filename instead.
  2. Query MB by `artist:"..." AND release:"..."`.
  3. Score release-groups: exact normalised album-title match
     dominates (defeats "BRAT" → "Brat and it's completely
     different..." deluxe trap). Penalise titles containing
     "deluxe" / "expanded" / "anniversary" / "edition".
  4. Within the chosen RG, pick the release with the lowest track
     count of those >= a sane floor — that's almost always the
     original LP, not a deluxe / anniversary / box set.
  5. Multi-disc handling: number tracks continuously across media
     so the UI's `t.number` is always unique and monotonic.

`manualTracks: true` still opts out. `status` becomes
`"musicbrainz"` for filled records, with `musicbrainzReleaseGroupId`
recorded so future runs can short-circuit (TODO: use it).

Rate limit: MB caps anonymous requests at 1 / sec. We sleep
1.1 s between calls.
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

MB_BASE = "https://musicbrainz.org/ws/2"
UA = "rae-mobile-ui-fetcher/2.1 (joonas@hypehype.com)"

# Higher score = preferred. EP and Single still beat None so they
# still get picked up when no Album release-group exists.
TYPE_RANK = {"Album": 3, "EP": 2, "Single": 1}

# Words in a release-group / release title that strongly suggest
# this isn't the canonical original release.
EXPANDED_TOKENS = (
    "deluxe", "expanded", "anniversary", "edition",
    "remastered", "remaster", "reissue", "live", "demo",
    "complete", "box set", "ultimate", "special",
    "completely different",
)


def mb_get(path: str, params: dict, timeout: int = 25, retries: int = 3):
    """One GET against MB's JSON API. Retries 503/timeouts because
    MB's public mirror occasionally hiccups under load — a single
    transient failure shouldn't lose data for an album."""
    url = f"{MB_BASE}{path}?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(
        url, headers={"User-Agent": UA, "Accept": "application/json"}
    )
    last_err = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            if e.code in (503, 502, 504) and attempt < retries - 1:
                time.sleep(2.0 * (attempt + 1))
                last_err = e
                continue
            raise
        except Exception as e:
            if attempt < retries - 1:
                time.sleep(2.0 * (attempt + 1))
                last_err = e
                continue
            raise
    if last_err:
        raise last_err
    return {}


def normalise(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", (s or "").lower())


def parse_stem(stem: str):
    """`02_mgmt-oracular_spectacular` -> ('MGMT', 'Oracular Spectacular')"""
    s = re.sub(r"^\d+_", "", stem)
    if "-" in s:
        a, b = s.split("-", 1)
        return a.replace("_", " ").strip(), b.replace("_", " ").strip()
    return s.replace("_", " ").strip(), ""


def expanded_penalty(title: str) -> int:
    low = (title or "").lower()
    return -5 if any(tok in low for tok in EXPANDED_TOKENS) else 0


def pick_release_group(groups, expected_artist: str, expected_album: str):
    """Score-based RG selection. Exact title beats partial beats
    type-based ranking. Penalise deluxe/expanded titles so the
    original release wins by default. Tolerant of small typos in
    the artist name via a shared-prefix fallback (e.g. filename
    `charlie_xcx` vs canonical `Charli xcx`)."""
    if not groups:
        return None
    want_a = normalise(expected_artist)
    want_t = normalise(expected_album)
    best = None
    best_score = -10**9
    for rg in groups:
        artists = " ".join(a.get("name", "") for a in rg.get("artist-credit", []))
        got_a = normalise(artists)
        got_t = normalise(rg.get("title", ""))

        a_score = 0
        if want_a and got_a:
            if want_a == got_a:
                a_score = 3
            elif want_a in got_a or got_a in want_a:
                a_score = 2
            else:
                # Shared 5-char prefix is a tolerant catch-all
                # for filename typos ("charlie" / "Charli",
                # "cyrys" / "cyrus", "Bjork" / "Björk" after
                # normalising → "bjork" / "bjrk", which is
                # close enough on prefix 4).
                k = 5 if min(len(want_a), len(got_a)) >= 5 else 4
                if want_a[:k] == got_a[:k]:
                    a_score = 1

        t_score = 0
        if want_t and got_t:
            if want_t == got_t:
                t_score = 5
            elif want_t in got_t or got_t in want_t:
                t_score = 2

        type_score = TYPE_RANK.get(rg.get("primary-type") or "", 0)
        penalty = expanded_penalty(rg.get("title", ""))
        # MB's own relevance score (0-100) as a tie-breaker only.
        mb_score = (rg.get("score", 0) or 0) / 100.0

        total = a_score * 10 + t_score * 8 + type_score + penalty + mb_score
        if total > best_score:
            best_score = total
            best = rg
    # Reject completely uncorrelated matches — e.g. asking for
    # "Crim3s" and getting a totally unrelated "Stay Ugly EP".
    # If neither the artist nor the title had any prefix overlap,
    # nothing in the result list is the album we're looking for.
    if best is None:
        return None
    best_artists = " ".join(a.get("name", "") for a in best.get("artist-credit", []))
    if (not want_a or not normalise(best_artists)
            or (want_a[:4] != normalise(best_artists)[:4]
                and want_a not in normalise(best_artists)
                and normalise(best_artists) not in want_a)):
        return None
    return best


def list_releases(rg_id: str):
    """All releases in an RG, with track counts. We pass `media`
    in `inc` so the response already carries the per-disc counts —
    one round-trip instead of N."""
    data = mb_get("/release/", {
        "release-group": rg_id,
        "fmt": "json",
        "limit": 100,
        "inc": "media",
    })
    return data.get("releases", []) or []


def release_track_count(rel) -> int:
    return sum((m.get("track-count") or 0) for m in (rel.get("media") or []))


def pick_canonical_release(releases):
    """Of all releases in an RG, pick the one most likely to be
    "the original album": Official > Promotional/Bootleg, no
    expanded-keyword in the title, lowest non-trivial track count,
    earliest date as a tie-breaker."""
    if not releases:
        return None
    scored = []
    for r in releases:
        tracks = release_track_count(r)
        if tracks <= 0:
            continue
        status = (r.get("status") or "").lower()
        status_score = 2 if status == "official" else 0
        title_penalty = expanded_penalty(r.get("title", ""))
        disambig_penalty = expanded_penalty(r.get("disambiguation", ""))
        date = r.get("date") or ""
        # Bigger negative = worse. We want SMALL track counts but
        # not 0/1 unless that's all there is. Bias toward 8-20.
        track_score = 0
        if tracks < 4:
            track_score = -2
        elif tracks <= 20:
            track_score = 3
        elif tracks <= 30:
            track_score = 1
        else:
            track_score = -2
        score = (status_score * 10 + track_score * 5
                 + title_penalty + disambig_penalty)
        scored.append((score, date, r))
    if not scored:
        return None
    # Sort: highest score first, then earliest date.
    scored.sort(key=lambda t: (-t[0], t[1] or "9999-99-99"))
    return scored[0][2]


def fetch_release_tracks(rel_id: str):
    """Get the full tracklist of a specific release. Numbers
    continuously across media (multi-disc albums become
    1..N rather than 1..k, 1..m)."""
    full = mb_get(f"/release/{rel_id}", {"inc": "recordings", "fmt": "json"})
    tracks = []
    for media in full.get("media", []) or []:
        for t in media.get("tracks", []) or []:
            ms = t.get("length") or 0
            tracks.append({
                "number": len(tracks) + 1,
                "title": t.get("title") or "",
                "durationMs": int(ms),
            })
    return tracks, full


def search_release_group(artist: str, album: str):
    bare_album = re.sub(
        r"\s*[-–—]\s*(Single|EP)\s*$", "", album or "", flags=re.I
    ).strip()
    q = f'artist:"{artist}" AND release:"{bare_album}"'
    data = mb_get("/release-group/", {"query": q, "fmt": "json", "limit": 10})
    return pick_release_group(data.get("release-groups", []), artist, bare_album)


def main():
    if not os.path.isdir(ALBUMS_DIR):
        print(f"albums dir not found: {ALBUMS_DIR}", file=sys.stderr)
        sys.exit(1)

    stems = sorted(
        d for d in os.listdir(ALBUMS_DIR)
        if os.path.isdir(os.path.join(ALBUMS_DIR, d))
    )
    print(f"Enriching {len(stems)} albums via MusicBrainz...", file=sys.stderr)

    counts = {"ok": 0, "no_match": 0, "manual_skip": 0}
    for i, stem in enumerate(stems, 1):
        path = os.path.join(ALBUMS_DIR, stem, "album.json")
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            data = json.load(f)

        if data.get("manualTracks") is True:
            counts["manual_skip"] += 1
            print(f"[{i}/{len(stems)}] {stem}: manual tracks (skip)", file=sys.stderr)
            continue

        # Candidate (artist, album) pairs in priority order. The
        # folder STEM is ground truth for albums where iTunes
        # matched the wrong thing (Pablo Honey → boy pablo, BRAT
        # → Guess - Single). But stem parsing falls down on
        # filenames without a dash (`bjork_post.jpg`, `crim3s_
        # stay_ugly.jpg`) and on filename typos
        # ("cyrys" / "Cyrus"). In those cases the iTunes-derived
        # album.json fields are usually correct, so we try them
        # as the fallback. Both candidates get one round-trip;
        # the first to return tracks wins.
        stem_artist, stem_album = parse_stem(stem)
        json_artist = data.get("artist") or ""
        json_album = data.get("album") or ""
        # Three candidate queries in priority order:
        #   1. stem artist + stem album — pure filename truth
        #   2. json artist + stem album — covers filename typos
        #      ("charlie xcx" / "miley cyrys") while preserving
        #      the filename's album-name truth
        #   3. json artist + json album — full fallback for
        #      filenames without a dash (`bjork_post.jpg`)
        # The middle slot was critical for BRAT: stem says
        # "charlie xcx / brat", json says "Charli xcx / Brat and
        # it's completely different..." (a prior bad cache). The
        # middle (Charli xcx / brat) is what we actually want.
        candidates = []
        seen = set()
        for a, b in (
            (stem_artist, stem_album),
            (json_artist, stem_album),
            (json_artist, json_album),
        ):
            if not a or not b:
                continue
            key = (normalise(a), normalise(b))
            if key in seen:
                continue
            seen.add(key)
            candidates.append((a, b))
        if not candidates:
            counts["no_match"] += 1
            print(f"[{i}/{len(stems)}] {stem}: no artist/album to search", file=sys.stderr)
            continue

        rg = None
        used_q = ""
        for cand_artist, cand_album in candidates:
            try:
                rg = search_release_group(cand_artist, cand_album)
            except Exception as e:
                print(f"[{i}/{len(stems)}] {stem}: search error — {e}", file=sys.stderr)
                rg = None
            time.sleep(1.1)
            if rg:
                used_q = f"{cand_artist} - {cand_album}"
                break

        if not rg:
            counts["no_match"] += 1
            tried = "; ".join(f"'{a} - {b}'" for a, b in candidates)
            print(
                f"[{i}/{len(stems)}] {stem}: no MB match (tried {tried})",
                file=sys.stderr,
            )
            continue

        # Pick a canonical release and walk down the list when the
        # top pick is missing track durations (some MB releases
        # are catalogue stubs with no recording metadata). Stop
        # as soon as we find a release whose tracks sum > 0.
        try:
            releases = list_releases(rg["id"])
            time.sleep(1.1)
        except Exception as e:
            print(f"[{i}/{len(stems)}] {stem}: list releases failed — {e}", file=sys.stderr)
            releases = []

        # Build a candidate-release queue ordered by canonical-ness.
        scored = []
        for r in releases:
            tc = release_track_count(r)
            if tc <= 0:
                continue
            status = (r.get("status") or "").lower()
            status_score = 2 if status == "official" else 0
            title_penalty = expanded_penalty(r.get("title", ""))
            disambig_penalty = expanded_penalty(r.get("disambiguation", ""))
            if tc < 4:
                track_score = -2
            elif tc <= 20:
                track_score = 3
            elif tc <= 30:
                track_score = 1
            else:
                track_score = -2
            score = (status_score * 10 + track_score * 5
                     + title_penalty + disambig_penalty)
            scored.append((score, r.get("date") or "9999-99-99", r))
        scored.sort(key=lambda t: (-t[0], t[1]))

        tracks = []
        for _, _, rel in scored[:4]:
            try:
                candidate_tracks, _ = fetch_release_tracks(rel["id"])
            except Exception as e:
                print(f"[{i}/{len(stems)}] {stem}: track fetch error — {e}", file=sys.stderr)
                candidate_tracks = []
            time.sleep(1.1)
            total = sum(t["durationMs"] for t in candidate_tracks)
            if candidate_tracks and total > 0:
                tracks = candidate_tracks
                break
            # Hold onto the best zero-duration result as a
            # last-resort fallback — better to show titles with
            # `0:00` than nothing.
            if candidate_tracks and not tracks:
                tracks = candidate_tracks

        if not tracks:
            counts["no_match"] += 1
            print(
                f"[{i}/{len(stems)}] {stem}: rg matched ({used_q}) but no usable tracks",
                file=sys.stderr,
            )
            continue

        canonical_artist = ", ".join(a.get("name", "") for a in rg.get("artist-credit", []))
        canonical_album = rg.get("title") or stem_album
        first_date = rg.get("first-release-date") or ""
        year = 0
        m = re.match(r"^(\d{4})", first_date)
        if m:
            year = int(m.group(1))

        total_ms = sum(t["durationMs"] for t in tracks)
        if canonical_artist:
            data["artist"] = canonical_artist
        data["album"] = canonical_album
        if year:
            data["year"] = year
        data["trackCount"] = len(tracks)
        data["tracks"] = tracks
        data["albumDurationMs"] = total_ms
        data["status"] = "musicbrainz"
        data["musicbrainzReleaseGroupId"] = rg["id"]
        data.pop("manualReason", None)

        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
            f.write("\n")

        counts["ok"] += 1
        secs = total_ms // 1000
        print(
            f"[{i}/{len(stems)}] {stem}: {len(tracks)} tracks, "
            f"{secs // 60}:{secs % 60:02d}  ({canonical_artist} — {canonical_album})",
            file=sys.stderr,
        )

        time.sleep(1.1)

    print(
        f"\nDone. ok: {counts['ok']}  no_match: {counts['no_match']}  "
        f"manual_skip: {counts['manual_skip']}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
