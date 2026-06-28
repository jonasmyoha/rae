# Live (bytecode VM) — status: FROZEN (2026-06-28)

**The Live (bytecode VM) target is deprecated/frozen for the time being.**

- **Don't touch the VM** and don't invest effort working around its gaps.
  Least friction is the goal for now.
- **Nothing is being removed.** The VM, its compiler, and all VM code stay in
  the tree exactly as-is — this is a freeze, not a deletion.
- **Default to the Compiled (C backend) target** for new examples and features.
  Do not add `open`/bare-call workarounds purely to keep Live working.
- Known Live-only gaps (qualified `module.func()` calls failing in the VM,
  synchronous spawn, `List(Struct).at`, extern-binding two-backend rule, etc.)
  are **no longer blockers** while this freeze holds.

If/when Live is revived, revisit QUEUE #108 (qualified-call Live gap) and the
related VM gaps first.
