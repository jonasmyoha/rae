# Repository Statistics

The `stats/` directory holds time-series data and tooling outputs for repository-wide metrics (line counts, performance runs, etc.).

Current files:

- `compiler_metrics.jsonl` – JSON Lines log of compiler source file/line counts, appended by `tools/stats/update_metrics.sh`.

## Updating metrics

Run the stats tool from the repo root before each commit (after builds/tests) to append a fresh data point (it only writes when file/line counts change):

```bash
./tools/stats/update_metrics.sh
```

This ensures we can chart code size and other future metrics over time as the Rae compiler grows.
