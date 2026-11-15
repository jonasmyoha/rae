# 151 Rae Perf: the capture viewer

A viewer for `.raeperf` capture files (docs/perf-tools-design.md §5, §8.2). A program writes one
with `RAE_PROFILE=1 RAE_PERF_OUT=<file>` (lib/Profile), or from the perf HUD's "Save last 10 s"
button (the debug overlay's Performance row).

```sh
compiler/bin/rae run examples/151_perfViewer/Main.rae                         # the walker sample
RAE_PERF_OPEN=capture.raeperf compiler/bin/rae run examples/151_perfViewer/Main.rae
RAE_PERF_VIEW=timeline compiler/bin/rae run examples/151_perfViewer/Main.rae  # open on a view
RAE_PERF_COMPARE=other.raeperf compiler/bin/rae run examples/151_perfViewer/Main.rae  # compare
RAE_PERF_OPEN=examples/151_perfViewer/assets/ab-walker.raeperf RAE_PERF_VIEW=compare \
  compiler/bin/rae run examples/151_perfViewer/Main.rae                          # an RAE_AB run
```

## Comparing two captures

With a second capture every view shows both, each series, lane, flame root and row marked " A" or
" B", and the Compare tab lists every zone of the program's own threads: its mean in each, the
change, the change in spreads (against one frame's scatter), the change in standard errors of the
two means ("certainty": a few or more is the program's, not chance), p95 and spread in each,
most certain first (`lib/perf/PerfCompare`, case 1478).

B is the file `RAE_PERF_COMPARE` names, or, when the opened capture is of an `RAE_AB` run, its
settings: the baselines are A and the first case B (`lib/perf/PerfSplit` splits it by the run's
`ab.case` counter; `lib/Profile` also writes the split files beside `RAE_PERF_OUT`, see
docs/profiling.md).

The tabs choose the view:

| view | shows |
|---|---|
| Frames | each frame's time (from the "frame start" marks), the CPU's and GPU's time where recorded (`frame.cpu.us`, `gpu.busy.us`), and how the frame times spread against the budget, with p50 and p95 |
| Timeline | every lane that recorded: its zones at their depths, the frames as marks; the wheel zooms, a drag pans, a click selects |
| Flame graph | each zone's own time along its lane and call path |
| Threads | each lane's busy, waiting and idle share of the capture (overlapping spans count once) |
| GPU | the GPU's buffer and texture memory and the draws, and the GPU lanes' spans (Metal command buffers, passes) |
| Audio | the audio render's share of its period, its time and underruns, and the audio lanes' spans |
| Memory | the resident size and the buffers' bytes; allocations a frame and the buffers alive |
| Systems | every zone of the program's own threads: count, total, mean, p95, maximum and spread |

The numbers come from `lib/perf/PerfCapture` (pure functions over the file's events, checked by
case 1477); `Viewer.rae` puts them into the `lib/ui` chart widgets laid out in
`assets/Viewer.raescene`.

## Samples

`assets/walker.raeperf` is 1.5 s of example 143 (the walker in the forest) headless on a Mac:
its systems and passes, the Metal command buffers, memory and GPU counters.
`assets/ui.raeperf` is 2 s of example 106 (the mobile UI). `assets/ab-walker.raeperf` is 143 under
`RAE_AB="ssao" RAE_AB_HOLD_SECONDS=3` (a baseline, ssao off, a baseline: 9 s). All are in Git LFS. To record one
again:

```sh
RAE_PROFILE=1 RAE_PROFILE_SECONDS=1.5 RAE_PERF_OUT=$PWD/examples/151_perfViewer/assets/walker.raeperf \
  RAE_HEADLESS_FRAMES=150 compiler/bin/rae run examples/143_walker_forest/Main.rae
```

A new sample changes the views: write the reference pictures again
(`bash examples/151_perfViewer/tools/references.sh`), look at each, and update the counts the
gate expects in `compiler/tools/run_examples.sh`.

## The gate

`RAE_EXAMPLE_FILTER=151_perfViewer make -C compiler test-examples` renders each view of the
walker sample, and the Compare view of the A/B sample, for 10 headless frames and compares each
with `references/<view>.png`.

## Not yet

Live programs (`RAE_PERF_LIVE`), folders of captures, the threads' critical path, the audio render plan's nodes and an iPad run are queued in
QUEUE.md.
