#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdbool.h>
#include <stddef.h>

/* A minimal build progress display for `rae run` / `rae build` on a terminal:
 *
 *   [........            ] sema
 *
 * One line, redrawn in place: twenty cells filled from
 * the share of the build each phase took LAST time (kept under the app's
 * `.rae/apps/<app>/` directory; sensible defaults before the first build) and
 * how far into the current phase we are by elapsed time — the module loader
 * also reports modules as they load. A phase never shows complete until it
 * is, so the bar under-promises rather than over-promises.
 *
 * Shown only when stderr is a terminal and RAE_PROGRESS is not `off`, so
 * pipes (the test runner, `rae watch` children) never see it. A driver that
 * holds stderr in a pipe and wants to draw its own bar sets
 * RAE_PROGRESS=lines: nothing is drawn, and instead a machine-readable line
 *
 *   @@RAE_BUILD_PROGRESS@@ phase=<load|sema|emit|cc> fraction=<0..1> elapsed_ms=<n>
 *
 * goes out every 500 ms and at each phase change (the devtools does this).
 * A ticker thread redraws every 200 ms; diagnostics call progress_hide()
 * first so an error line is never written over the bar. */

typedef enum {
  PROGRESS_LOAD = 0,   /* module graph: lex + parse every module */
  PROGRESS_SEMA,       /* semantic analysis of the merged module */
  PROGRESS_EMIT,       /* C emission */
  PROGRESS_CC,         /* the system C compiler + link */
  PROGRESS_PHASES
} ProgressPhase;

/* Start the display (no-op when it should not show). `record_path` is the
 * file the last build's phase durations are read from and this build's are
 * written to (NULL: defaults only, nothing written); `last_phase` is the
 * phase this pipeline ends with, so an emit-only build fills the bar without
 * a C compile it will never run.
 *
 * How far into the CURRENT phase the bar is comes from the phase itself when
 * it can count its work (progress_work: sema per declaration, emission per
 * discovery walk and per function body) and from elapsed time against the
 * last build only for a phase that cannot (the C compiler). A time estimate
 * that overruns its budget creeps on asymptotically instead of freezing, so
 * the bar keeps moving through a phase that got slower since last time. */
void progress_begin(const char* record_path, ProgressPhase last_phase);

/* Enter a phase; the previous phase is recorded as complete. */
void progress_phase(ProgressPhase phase);

/* The loader reports each module as it finishes loading. */
void progress_module_loaded(int loaded);

/* The current phase reports how far along it is by WORK: it is in a stage
 * spanning [lo, hi] of the phase (each 0..1) and has finished `done` of that
 * stage's `total` items. The bar only moves forward within a phase, so a
 * stage whose item count is still growing (specializations discovered while
 * emitting) never pulls it back; a phase that never reports is estimated by
 * elapsed time instead. Cheap enough to call per item. */
void progress_work(double lo, double hi, size_t done, size_t total);

/* Take the bar off the screen before printing something else to stderr. The
 * ticker draws it again shortly after. Safe to call when not showing. */
void progress_hide(void);

/* Stop the display and clear it. On success the phase durations of this build
 * are written as the estimate for the next one. */
void progress_end(bool ok);

#endif /* PROGRESS_H */
