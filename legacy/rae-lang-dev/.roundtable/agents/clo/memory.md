# Clo Memory — Monomorphisation Stabilization (FINAL)

## Final Status (2026-03-20, checkpoint 8)
- **Unit: 172/174 (98.9%), Examples: 41/45 (91.1%)**
- **Combined: 213/219 (97.3%)**
- Started: 143/219 (65.3%) — **recovered 70 tests across 38 tasks**

## Remaining 6 Failures
All share cross-specialization method resolution issue:
1. 370_map_basic — void specialization + map method V
2. 371_string_overhaul — const void* designated init
3. 21_stdlib_demo — toInt method overload
4. 28_crypto_demo — sys variable scope
5. 94_tetris2d — cross-type createList return
6. list_native_any — auto-box for T=RaeAny

## Root Cause for Further Work
Generic template body shared across specializations. Inner
decl_link fixed to first specialization. Need per-specialization
body cloning with fresh call resolution in sema.
