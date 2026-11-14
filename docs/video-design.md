# Video in Rae — moved

Video is not part of this repository. Since 2026-11-14 it is a set of Rae
packages in its own public repository, **raeVideo**
(https://github.com/jonasmyoha/raeVideo): `packages/video` (the Media API,
the frame pipeline, the image passes a player needs, a simple player) and
`packages/ffmpeg` (FFmpeg as LGPL, opt-in). Rae programs take them as git
dependencies at a released tag (`video/v<version>`, `ffmpeg/v<version>`).

The design that was here is now raeVideo's
[`docs/design.md`](https://github.com/jonasmyoha/raeVideo/blob/main/docs/design.md),
with what lives where in
[`docs/scope.md`](https://github.com/jonasmyoha/raeVideo/blob/main/docs/scope.md).
**Its section numbers are the same as this document's were**, so a citation in
this repository such as "video-design.md §3.4" or "§6.3" means that section of
raeVideo's `docs/design.md`.

What stays in this repository, because the engine or other programs use it
too: `lib/media` (MP4/MOV boxes, `MediaTime`), `lib/webgpu` and the render
graph, `lib/color` (raeVideo's design §8.2: the renderer uses it, so it
stays here), and `packages/audio`, the clock video follows.

## 12. Decisions still needed

Only V2 is kept here, because it changed where the work happens; V1 and V3 to
V6 are in raeVideo's `docs/design.md` §12.

- **V2 — where video and the permissive libraries live.** Decided 2026-11-11
  as `packages/video` in this repository, like `packages/audio`
  (`audio-design.md` §13 D6). **Changed 2026-11-14:** this repository takes
  only engine-level work, so video is `packages/video` in raeVideo, declaring
  this repository's `audio` package as its git dependency at a released
  version. dav1d, libvpx and OpenEXR are used as libraries in that package's
  `native/`; FFmpeg (LGPL) is its own package there, `packages/ffmpeg` (which
  answers V1).
