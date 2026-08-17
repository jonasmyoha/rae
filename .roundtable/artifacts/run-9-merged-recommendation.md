# Merged recommendation

Before committing, grep `lib/core.rae` for the `let entry = rae_ext_rae_buf_get(...)`/`entry.field = ...`/`rae_ext_rae_buf_set(...)` pattern to confirm StringMap/IntMap don't rely on alias-by-let semantics, run `make -C compiler test`, and wire `--target live` plus a Live snapshot into `examples/98_mobile_ui/snapshot.sh` so this regression class is caught at commit time. Defer the larger `OP_SET_LOCAL` / `OP_WRITE_THROUGH_REF` split (Chattie's deeper cleanup) until items (1)–(3) are green.
