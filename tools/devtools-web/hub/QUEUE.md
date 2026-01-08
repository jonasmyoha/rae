- Only announce important events using `SAY: <message>` lines. Speak nothing else.

# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks

### T032 - Implement Standard Library Bootstrap & Random API
- Repo: rae
- Summary:
  - Implement `import nostdlib` detection.
  - Setup auto-import of `lib/core.rae` (if exists).
  - Implement a deterministic PRNG (like Xoshiro or a stable LCG) in `rae_runtime.c` and VM natives.
  - Expose `seed(n: Int)`, `random(): Float`, `randomInt(min: Int, max: Int): Int` in a new `lib/math.rae`.
  - Ensure Live and Compiled modes produce identical sequences for the same seed.
- Acceptance:
  - Hybrid test verifies deterministic output for a fixed seed across backends.
  - `import nostdlib` works as intended.

### T033 - Randomness and levels for Advanced Pong AI
- Repo: rae
- Summary:
  - Integrate `math` randomness into `advanced_pong`.
  - Randomly select an AI difficulty/level at the start of each match.
  - Make `reactionTimer` threshold and movement speed slightly random each "action".
- Acceptance:
  - AI difficulty varies between game restarts.
  - AI behavior is less mechanical and more unpredictable.
