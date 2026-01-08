- Only announce important events using `SAY: <message>` lines. Speak nothing else.

# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks

### T027 - Implement Cooked String Interpolation
- Repo: rae
- Summary:
  - Update lexer to handle `"foo {expr} bar"`.
  - Update parser to handle interpolated strings (likely as concatenation expression).
  - Enforce `{}` is error.
  - Handle escapes `\\{`, `\\}`, `\\"`, `\\\\
`.
- Acceptance:
  - `"{x}"` works.
  - `"Val: {x + 1}"` works.
  - `{}` fails.
  - Formatting preserves interpolation.

### T028 - Implement Trailing Comma Policy
- Repo: rae
- Summary:
  - Update formatter (`pretty.c`) to enforce:
    - Multiline lists -> MUST have trailing comma.
    - Singleline lists -> MUST NOT have trailing comma.
  - Apply heuristic (line length > 120 or complexity) to decide multi/single.
- Acceptance:
  - Long function calls wrap and get trailing comma.
  - Short ones stay single line without trailing comma.
  - `rae format` is deterministic.

### T029 - Implement Formatter Disable Pragmas
- Repo: rae
- Summary:
  - Support `# raefmt: off` and `# raefmt: on`.
  - Formatter preserves content verbatim in these blocks.
- Acceptance:
  - Code inside `off/on` block is not reformatted.
  - Formatter still processes surrounding code.

### T030 - Enhance Advanced Pong
- Repo: rae
- Summary:
  - Add scoring system to `advanced_pong`.
  - Add `UISystem` to draw scores at center top (e.g. `4 - 4`).
  - Increase ball speed on paddle hit in both `pong` and `advanced_pong`.
- Acceptance:
  - `advanced_pong` displays score.
  - Ball gets faster after hits in both examples.
  - Game is winnable (or at least gets harder).
