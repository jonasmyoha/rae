# Merged recommendation

Before committing: grep `lib/core.rae` to confirm `StringMap.set` / `IntMap.set` do not rely on alias-by-let semantics (the trailing `rae_ext_rae_buf_set(... entry)` strongly suggests they don't); run `make -C compiler test`; snapshot the mobile UI in both Compiled and Live; then wire `rae run --target live examples/98_mobile_ui/main.rae` into `examples/98_mobile_ui/snapshot.sh` so this regression class is caught at commit time.
