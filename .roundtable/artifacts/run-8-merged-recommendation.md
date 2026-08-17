# Merged recommendation

Then re-snapshot both the album and now-playing screens in Compiled mode to confirm the MGMT cover renders in its real colours and both pills regain their backgrounds. Defer the Live VM `view`-ref fix to a follow-up — but as part of this same PR, extend `vm.c:448`'s `diag_error` to print the current chunk name + bytecode offset so the next iteration on the Live failure has a real source location.
