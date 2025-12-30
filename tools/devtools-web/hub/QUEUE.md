# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks
### T004 - Add regression test for `rae format --write`
- Repo: rae
- Summary:
  - Create a new test case ensuring `rae format --write` writes canonical output to a temp file without stdout
  - Use existing test runner mechanics to clean temp files
- Acceptance:
  - `make test` includes the new case and passes
  - Failure output clearly indicates stdout/file mismatches if behavior regresses

### T005 - Document hub workflow in devtools README
- Repo: rae-devtools-web
- Summary:
  - Add a section to `README.md` describing the hub workflow (QUEUE → INPROGRESS → RESULTS, branch naming, acceptance logs)
  - Cross-link to hub files and clarify single-agent expectations
- Acceptance:
  - README section accurately reflects the current process with working links
  - No build/test changes required
