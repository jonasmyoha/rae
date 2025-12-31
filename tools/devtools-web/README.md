# Rae Compiler Development Dashboard

Lightweight developer dashboard that keeps Rae compiler contributors aware of build health, test coverage, and historical performance trends without relying on heavy frameworks. The current implementation lives in this repository so it can eventually be re-authored in Rae itself.

## Project Overview

- **Goal**: Provide a simple yet powerful dashboard for monitoring the Rae compiler (builds, tests, stats, Codex automation).
- **Constraint**: Keep everything transparent and portable so the entire stack can be rewritten in Rae once the language has UI primitives.
- **Scope**: Focus on developer productivity (build/test monitoring, stats, Codex hand-off). No attempt to replace IDE/editor tooling.

## Technology Stack

| Layer    | Choice                                                                                 | Rationale                                                                                     |
|----------|----------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| Runtime  | Bun (preferred) or Node.js                                                             | Fast startup, familiar ecosystem, easy to port                                               |
| Language | TypeScript                                                                             | Static safety without build-time complexity                                                  |
| Server   | Raw HTTP + WebSocket (Bun primitives or `ws`)                                           | Low abstraction overhead                                                                      |
| Storage  | SQLite via Bun's driver or `better-sqlite3`                                            | Single-file DB, easy backup/versioning                                                        |
| Client   | Vanilla JS / optional Preact micro-components + CSS variables                          | Minimal runtime, easy to inspect and port                                                    |
| Charts   | Chart.js                                                                               | Straightforward API                                                                           |
| Syntax   | Prism.js (or lightweight equivalent)                                                   | Highlight compiler/code snippets                                                             |

## Getting Started

### Prerequisites

- [Bun](https://bun.sh/) ≥ 1.0 (preferred) or Node.js ≥ 18
- Rae compiler repository available next to this project (default `../rae`)

### Install Dependencies

```bash
bun install
```

### Configuration

Copy `config.example.json` to `config.json` at the repo root (or set equivalent env vars) and tweak the commands/paths as needed. Example:

```json
{
  "compilerPath": "../rae",
  "buildCommand": "cd compiler && make",
  "testCommand": "cd compiler && make test",
  "cleanCommand": "cd compiler && make clean",
  "rebuildCommand": "cd compiler && make clean && make",
  "port": 3000,
  "examplesPath": "examples"
}
```

### Run the Dashboard

```bash
# Development server with auto-restart + client reload
bun run dev

# Production build (if/when a bundle step exists)
bun run start
```

`bun run dev` launches a small watcher that restarts the Bun server whenever files under `src/`, `config.json`, or `package.json` change. The browser auto-reloads after each restart, so you rarely need to refresh manually. The server exposes both HTTP and WebSocket endpoints; open `http://localhost:3000` in a browser.

### Use the Inline Test Runner

- Click **Run all tests** (or press `Ctrl+T`/`Cmd+T`) inside the dashboard to execute the configured test command from `config.json`. Be sure to set `testCommand` to a real script (current default assumes `cd compiler && make test`; adjust if your workflow differs).
- Live stdout/stderr output appears in the terminal panel; the run status chip flips to **Passed**/**Failed** when complete.
- Real-time per-test updates populate the suite summary and the test list so you can see which specific files passed/failed.
- The browser auto-reloads whenever the Bun dev server restarts, so you rarely need to manually refresh during local development.

### Use the Build Controls

- Buttons for **Build**, **Clean**, and **Rebuild** run whichever commands you defined in `config.json` (current defaults assume the Rae compiler Makefile lives under `../rae/compiler`; adjust if yours differs); `Ctrl+T/Cmd+T` still triggers tests, while `Ctrl+B/Cmd+B` triggers Build.
- Build stdout/stderr streams into its own terminal panel with copy button and status indicator.
- Rebuild executes the clean command followed by the build command (via `clean && build`) so you can reset the compiler tree before building again.

### Run the Examples

- The **Examples** panel scans the configured `examplesPath` (defaults to `../rae/examples`) and lists every `.rae` demo project.
- Selecting an example reveals its file list and enables the **Run example** button, which calls `bin/rae run <entry>` via the new bytecode VM.
- Output streams into a terminal next to the file tree; click any file button to view the highlighted source without leaving the dashboard.
- The data model already supports multi-file projects—if a folder contains a `main.rae`, it becomes the default entry; otherwise the first `.rae` file is used.
- Toggle **Watch** to run `bin/rae run --watch` so code changes (e.g., the `hot_reload.rae` sample) automatically recompile and stream logs. Use the inline editor + Save button to update the active file without leaving the dashboard.

### Track Stats

- Every test/build command stores metrics (durations, pass/fail counts) in `data/devtools.db` via SQLite.
- The **Stats preview** panel pulls recent entries from the database (default metrics: `tests.duration_ms`, `builds.duration_ms`) so you can see trends without leaving the dashboard.
- Data can be exported later for Chart.js visualizations; each entry carries metadata (`success`, run id, etc.).

### Browse Rae Test Sources

- The **Test sources** panel lists files from `testsPath` (default `compiler/tests`) and renders them with a lightweight Rae syntax highlighter.
- Select a file to see its contents; use the corner copy icon to grab the code for editor reproduction or bug reports.
- The highlighter loads `syntaxSummaryPath` (default `../rae/docs/rae_syntax.json`) for authoritative tokens—keep that file up to date as the language evolves.

### Error Log

- A warning icon in the header lights up when the dashboard encounters fetch/WebSocket/runtime errors; click it to open the error log for details.
- Use the log to capture errors (e.g., missing syntax file) before reporting issues—entries include timestamps and sources.

## Core Features

### Test Dashboard (Priority: High)

- File-system tree of Rae tests with real-time status (⚪ not run, 🟡 running, 🟢 pass, 🔴 fail, 🟠 error).
- Detailed view: expected vs. actual output diff, compiler stdout/stderr, stack traces.
- Controls for `Run All Tests`, `Run Failed`, `Run This Test`, `Clear Results`.
- WebSocket streaming for incremental updates; optimistic UI and sticky summary header.

### Build Controls (Priority: High)

- Buttons for `Build Compiler`, `Clean`, `Rebuild`.
- Terminal-style streaming panel with timestamps, ANSI-aware highlighting for warnings/errors, and auto-scroll.
- Status indicator with elapsed time tracking and cancellation support (future).

### Statistics Viewer (Priority: Medium)

- Records build/test durations, pass rate, file/line counts, binary size, and other metrics after each successful run.
- Uses SQLite schema:

  ```sql
  CREATE TABLE stats (
    id INTEGER PRIMARY KEY,
    timestamp TEXT NOT NULL,
    metric_name TEXT NOT NULL,
    metric_value REAL NOT NULL,
    metadata TEXT
  );
  CREATE INDEX idx_metric_time ON stats(metric_name, timestamp);
  ```

- Chart.js line charts with toggles, multiple axes, and CSV export (last 30 days by default).
- Manual "Snapshot Stats" button plus automatic post-build recording.

### Codex Integration (Priority: Low for MVP)

- "Ask Codex" button opens a panel with current failures/context and free-form prompt.
- Invokes the `codex` CLI inside the Rae compiler repo and streams output back into the UI.
- Optional templates to re-use common prompts.

### Modern UI/UX

- Dark theme by default with planned toggle.
- Consistent spacing via CSS variables, subtle shadows, and 200–300ms transitions.
- Keyboard shortcuts: `Ctrl+B` build, `Ctrl+T` run all tests, `Ctrl+R` re-run failed tests.
- Toast notifications and friendly error handling when Rae paths/commands are misconfigured.

## Architecture

```
rae-devtools/
├── src/
│   ├── server/
│   │   ├── main.ts          # HTTP server + websocket upgrade
│   │   ├── compiler.ts      # Build orchestration
│   │   ├── tests.ts         # Test runner + parsing
│   │   ├── stats.ts         # SQLite accessors
│   │   ├── codex.ts         # Wrapper around Codex CLI
│   │   └── websocket.ts     # Pub/sub for live updates
│   ├── client/
│   │   ├── index.html
│   │   ├── style.css
│   │   ├── app.js
│   │   └── components/
│   └── shared/
│       └── types.ts         # Shared DTOs and enums
├── data/                    # Houses SQLite DB (tracked via .gitkeep)
├── package.json
├── tsconfig.json
└── README.md
```

- **Backend**: Minimal HTTP handlers plus WebSocket broadcaster. Uses child processes for build/test commands and streams stdout/stderr line-by-line.
- **Frontend**: Single-page layout with sidebar test tree and main content area for build logs/stats/test detail.
- **Shared Types**: Keep WebSocket messages and HTTP responses type-safe and portable.
- **Data Folder**: Contains SQLite DB (`devtools.db` or similar). Only the `.gitkeep` file is tracked; DB files stay local.

## Extending Statistics

1. Add a metric descriptor in `src/server/stats.ts` (name, units, query strategy).
2. Produce measurements after builds/tests (e.g., parse CLI output or inspect the Rae repo).
3. Insert into SQLite with optional JSON metadata (`{ "commit": "...", "branch": "..." }`).
4. Update frontend Chart.js config to display/select the metric.
5. Document the new metric in this README so contributors understand how it is sourced.

## Development Phases

1. **Core Infrastructure** – project scaffolding, HTTP server, WebSocket plumbing, simple HTML shell.
2. **Test Dashboard** – command execution, parser, tree UI, diff view.
3. **Build Controls** – build/clean tooling with streaming output and status tracking.
4. **Statistics** – SQLite integration, data recording, and visualization widgets.
5. **Polish** – keyboard shortcuts, theming, Codex integration, accessibility.

## Testing & Quality

- Unit tests for log parsing, stats persistence, and message serialization (to be added in `src/server/__tests__`).
- Manual checklist:
  - Build runs end-to-end and updates status indicator.
  - Full test suite plus "Run Failed" scenario.
  - SQLite stats entries appear after builds/tests and persist across restarts.
  - WebSocket reconnect handles server restarts gracefully.
  - UI remains responsive during long-running operations.

## Future Ideas

- Run arbitrary Rae programs from the dashboard.
- Performance profiling overlays once Rae exposes profiling hooks.
- Benchmark suite tracking with regressions alerts.
- Git metadata correlation (stats + commits).
- Multi-project dashboards if Rae spawns additional repos.
- Plugin system for community metrics or custom panels.

## Contributing

1. Fork/clone this repo alongside the main Rae compiler repo.
2. Configure the path/commands in `config.json`.
3. Work on one phase/feature at a time; keep abstractions simple.
4. Commit meaningful increments frequently (aim for 1–10 commits per day) and push once changes build/run cleanly.
5. Open PRs with screenshots or terminal captures so reviewers can visualize the UI.

## Hub Workflow

The lightweight hub under [`hub/`](hub/) coordinates all Rae work (compiler and
devtools) via numbered tasks:

1. Queue a task in [`hub/QUEUE.md`](hub/QUEUE.md) with a short summary and
   acceptance checks. Every task uses the `T### - title` format.
2. Move the task to [`hub/INPROGRESS.md`](hub/INPROGRESS.md) once you start
   working. Create a branch named `agent/<repo>/T###-slug` in the relevant repo.
3. Log starts, completions, and verification commands in
   [`hub/LOG.md`](hub/LOG.md) so the UI (and humans) can track progress.
4. When finished, record the outcome in `hub/RESULTS/T###.md` (summary + tests),
   run the acceptance checks, and remove the task from the queue.

The single-agent setup simply repeats these steps task-by-task, ensuring the
devtools dashboard and the CLI stay in sync with the latest automation work.

## License

Apache 2.0 (see `LICENSE` file).
