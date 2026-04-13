# Clo Memory — Monomorphisation Stabilization

## Status (2026-03-20, final)
- **Unit: 173/174 (99.4%), Examples: 41/45 (91.1%)**
- **Combined: 214/219 (97.7%)** — recovered 71 tests

## Remaining 5 Failures
1. 370_map_basic — buf_get returns RaeAny, map entries expect concrete
2. 21_stdlib_demo — opt String let type vs concrete String value
3. 28_crypto_demo — sys identifier undeclared
4. 94_tetris2d — cross-type specialization
5. list_native_any — T=RaeAny callers don't BOX args

## Key Fixes This Session
- Method overload resolution searches ALL decls by receiver type
- Non-generic method overloads now check this param type
- Buffer/Any view params no longer get & or prim_view_wrap
- Void generic specializations blocked from registration
- 371_string_overhaul FIXED (fromCStr view Buffer)

## Git Config
Remote URLs use github.com-jonaskivi for SSH key routing.
