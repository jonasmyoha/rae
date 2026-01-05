# Rae Devtools — Live / Compiled / Hybrid Support Plan

## Goals
1. Let contributors run compiler **tests** and **builds** in Live, Compiled, or Hybrid mode straight from the dashboard without shell gymnastics.
2. Extend the **Examples** panel so every sample can be executed in multiple targets (Live, Compiled, Hybrid) and introduce at least one hybrid-specific demo that exercises hot-reload / “downloaded code” workflows.
3. Keep the UX explicit: users should always know which target/profile a command uses, and it should be easy to switch between them.

These additions build on the current dashboard features described in `README.md` (Run all tests, Build controls, and Example runner) by layering target-aware commands and surfacing hybrid-specific flows.

## Status
- Target-aware commands now exist end-to-end. `config.json` exposes a `targets` array, the server broadcasts target metadata in every Test/Build/Example event, and the client renders synchronized selectors (persisted via `localStorage`).
- Example runs support Live/Compiled/Hybrid commands, including a dedicated **Build artifacts** action that dumps file sizes + hashes (mirroring the hybrid regression tests).
- `devtools.json` metadata lets examples describe supported targets + default recommendations. The new `examples/hybrid_hot_reload` package ships with metadata and serves as the first hybrid-specific demo inside the dashboard.
- Remaining work: automation for “downloaded code” simulations and additional short-running Compiled/Hybrid test suites to expose in the target dropdown.
- The hybrid demo now wires a "Simulate download" helper through example metadata; the dashboard renders dev/release action buttons that run the helper script, stage `.vmchunk` files under `.simulated_downloads/`, and stream the manifest/hash summary back to the UI.
- The Examples panel surfaces a **Staged downloads** list that enumerates `.simulated_downloads/<profile>/<version>/<timestamp>` outputs (size + hash) so reviewers can immediately see which bundles are available for the host to reload.

## Current Baseline
- `config.json` stores single `buildCommand` / `testCommand` values, so the UI can only run one configuration at a time.
- The Examples panel shells out to `bin/rae run <entry>` (optionally `--watch`), which implies Live-only execution.
- There is no notion of hybrid packages or Compiled builds inside the dashboard yet; contributors must pivot to a terminal to run `rae build --target ...`.

## Proposed Additions

### 1. Target-Aware Commands
- Extend `config.json` with a `targets` array. Each target entry contains:
  ```json
  {
    "id": "live",
    "label": "Live (bytecode)",
    "testCommand": "cd compiler && make test TARGET=live",
    "buildCommand": "cd compiler && bin/rae build --target live --out build/devtools.vmchunk examples/hello.rae",
    "exampleCommand": "cd compiler && bin/rae build --target live --out {OUT} {ENTRY}"
  }
  ```
- Update the Build + Test panels to render a segmented control (or dropdown) listing all configured targets. Selecting a target swaps the command that is executed when the user clicks **Run tests**, **Build**, **Rebuild**, etc.
- Persist the last-selected target in `localStorage` so browser refreshes keep the same mode.
- Replace the README instructions for “Inline Test Runner” and “Build Controls” with notes about the new selector so contributors know to configure per-target commands.

### 2. Example Runner Enhancements
- Augment the server’s example metadata with `supportedTargets` (default: `["live"]`). For each target, specify the command template used to run the example:
  - `live`: `bin/rae run {ENTRY}`
  - `compiled`: `bin/rae build --target compiled --emit-c --out {TMP}/main.c {ENTRY}`
  - `hybrid`: `bin/rae build --target hybrid --out {TMP}.hybrid {ENTRY}`
- In the client UI, add a target selector next to **Run example**. When hybrid/native is chosen:
  - Stream the CLI output into the terminal as usual.
  - After a successful build, show download buttons for the generated artifacts (e.g., zipped `.hybrid` folder or `out.c`) so users can inspect them without leaving the browser.
- Respect the existing **Watch** toggle by constraining it to Live targets (Compiled/Hybrid builds remain single-shot).

### 3. Minimal Hybrid Demo Workflow
- Add a new example folder inside `../rae/examples` (e.g., `examples/hybrid_hot_reload/`) containing:
  - `host.rae`: minimal CLI or stub that simulates a Compiled host embedding the Live VM.
  - `scripts/downloaded/` with small Rae snippets that mimic remote updates.
  - README describing the flow (“host compiles hybrid bundle, loads VM chunk, then replaces code when a new `.vmchunk` appears”).
- In devtools, tag this example as “Hybrid demo” and surface additional controls:
  - **Build hybrid package** – runs `rae build --target hybrid` into a temp dir, then lists the produced files with hashes (mirroring the test harness summary).
  - **Simulate download** – copies a pre-built `.vmchunk` into the running host process’ watch directory so the Live VM reload path can be observed within the dashboard logs.
  - **Release vs Dev profile** toggles – call `--profile dev` or `--profile release` accordingly to show how manifests / runtimes differ.

### 4. Hybrid Test Coverage
- Introduce dedicated regression scenarios under `compiler/tests/` (already started with `407_build_live` and `408_build_hybrid`). The devtools test panel can expose a “Target” dropdown that defaults to the compiler’s Live make target but also offers:
  - **Compiled smoke** – run a slim subset of build tests using `rae build --target compiled --emit-c`.
  - **Hybrid sanity** – exercises the new `.hybrid` packaging tests so dashboard users can verify bundle integrity.
- For each target, stream the same diff/console output already available in the Tests panel; failed runs should clearly state the target.

### 5. “Downloaded Code” Simulation
- Add a helper command (`config.json` property like `hybridHotReloadCommand`) that runs a script simulating remote code arrival:
  1. Builds a `.hybrid` bundle into a watch folder.
  2. Notifies the dashboard via WebSocket so the UI can highlight “New hybrid package detected”.
  3. The running host example automatically reloads the `.vmchunk` (similar to `bin/rae run --watch`).
- Provide a “Simulate download” button in the Example detail view that invokes this helper, so reviewers can see the reload log sequence without leaving the UI.

## Implementation Steps
1. **Config Schema + UI Selectors**
   - Update `config.example.json` with a `targets` array.
   - Modify Build/Test APIs to accept target IDs and dispatch the corresponding commands.
   - Add target selectors in the client panels; persist selection locally.
2. **Example Metadata + Runner**
   - Extend server example scanning to read optional `devtools.json` per example (declares entry file + supported targets).
   - Implement hybrid/native execution paths in the runner service.
3. **Hybrid Demo Assets**
   - Land the new `examples/hybrid_hot_reload/` package inside the compiler repo.
   - Teach the dashboard to recognize this example and expose extra buttons (build, simulate download).
4. **Testing Hooks**
   - Level up the Tests panel to let users choose “Live / Compiled / Hybrid” before launching `make test`.
   - Surface the target name in run summaries and history entries.
5. **Docs + Onboarding**
   - Update `README.md` (devtools) with instructions on configuring targets and using the hybrid demo workflow.
   - Document how to interpret bundle materialization (hashes, file layout) so contributors can verify outputs without running CLI commands manually.

## Open Questions
- Should the dashboard automatically zip `.hybrid` directories for download, or is listing the contents sufficient?
- How do we sandbox potentially large native builds? (Initial approach: limit native/hybrid example runs to pre-approved samples with short build times.)
- Do we want target-aware metrics in SQLite (e.g., `tests.vm.duration_ms`) to compare performance across modes? If so, extend the stats schema accordingly.

Answering these during implementation will help ensure the devtools UI remains simple while unlocking the hybrid workflows needed for Live/Compiled parity.
