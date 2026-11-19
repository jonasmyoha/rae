# A loop that steps by more than one: the options

Status: decided 2026-11-20: option (c), `+=` and `-=` (to match `++` and `--`), built in Rae
0.4.28 (docs/SYNTAX_RULES.md §4). The evaluation below is kept as it was written; its
recommendation was (b).

## What happens today

Rae has three loop forms:

| form | uses in the tree | example |
|---|---|---|
| counted: `loop var i: T = a, cond, step { }` | about 4,200 | `loop var i: Int = 0, i < n, ++i { }` |
| over a list: `loop let x: T in list { }` | about 1,650 (`loop let`/`var ... in`) | `loop let node: view Node in nodes { }` |
| while: `loop cond { }` | about 750 | `loop i < 16 { ... i = i + 1 }` |

The counted form's third part is parsed as an expression
(compiler/src/parser.c, `stmt->as.loop_stmt.increment = parse_expression(parser)`) and emitted as
the third part of a C `for`. Assignment in Rae is a statement (`AST_STMT_ASSIGN`), not an
expression, and there is no `+=`. So:

- `++i` and `--i` work (about 4,160 and 33 uses).
- `i = i + 5` does not parse: the expression ends before `=`, and the error is "expected '{' to
  start block", followed by a confusing second error on the next line.
- Any other expression parses and is silently useless: `loop var node: Int = first, node >= 0,
  nextNode(node: node) { }` compiles, calls `nextNode` each turn, throws its answer away, and
  never ends.

Nothing in docs/SYNTAX_RULES.md says the step is limited to `++`/`--` on purpose. game proto2 hit
it and counted steps instead.

## The four examples in today's Rae

```rae
# 1. a step of 5: count the steps
loop var step: Int = 0, step <= (last - first) / 5, ++step {
  let degrees: Int = first + step * 5
}
# 2. a step of -2 going down: the same
loop var step: Int = 0, step <= top / 2, ++step {
  let value: Int = top - step * 2
}
# 3. a Float step of 0.25: count, and the value has no drift
loop var step: Int = 0, step <= 8, ++step {
  let t: Float = step.toFloat() * 0.25
}
# 4. walking a chain: the while form
var node: Int = first
loop node >= 0 {
  visit(node: node)
  node = nextNode(node: node)
}
```

## The options

### (a) Keep it; make the error say what is allowed

- **Parser:** after the step expression, accept only `++name` / `--name` of the loop's variable
  (or any `++`/`--`); anything else is an error: "a loop's step is ++i or --i; to step by n, count
  steps (`first + step * n`), or use `loop cond { ... i = i + n }`". This also closes the silent
  case (a bare call as the step).
- **Codegen, formatter:** no change.
- **Programs touched:** none that build today (a check of the tree for steps other than
  `++`/`--` would confirm: today the 4,194 counted loops all use them).
- **Fit:** one way to count; values derived from a counter have no Float drift (example 3).
  Costs a line in the body and a little arithmetic in the condition, which is where game proto2's
  example became harder to read than `degrees = degrees + 5`.

### (b) The step may be an assignment statement

`loop var degrees: Int = first, degrees <= last, degrees = degrees + 5 { }`

- **Parser:** the third part is parsed as a simple statement: `++i`, `--i`, or an assignment
  `place = expr` (no `let`, no call on its own, no block). The AST keeps a statement there instead
  of an expression (`loop_stmt.step`), or wraps the assignment.
- **Codegen:** the C `for`'s third part takes an assignment expression (`degrees = degrees + 5`):
  the C backend already emits assignments; it emits this one in the `for` header, or as the last
  statement of the body with `continue` jumping to it (the backend already keeps a label for a
  counted loop's `continue`, c_stmt.c around the `for (; condition; increment)` emission).
- **Sema / flow:** the increment's checks (moves, captured names, may-wait) look at a statement
  instead of an expression: the same helpers exist for statements.
- **Formatter:** prints the statement.
- **Programs touched:** none (only new code uses it).
- **Fit:** one place for the step, the way the reader expects it. It keeps statements and
  expressions apart (the step is a statement slot, not assignment as an expression). Covers
  examples 1, 2, 3 (a Float counter, with the usual drift) and 4
  (`loop var node: Int = first, node >= 0, node = nextNode(node: node)`). It makes the counted
  loop and the while loop overlap more (example 4 can be written both ways).

### (c) Compound assignment `+=` / `-=`, allowed as the step

`loop var degrees: Int = first, degrees <= last, degrees += 5 { }`

- **Lexer, parser:** new tokens `+=`, `-=` (and then the question of `*=`, `/=`, `%=`, `<<=` ...),
  a new statement kind or sugar for `x = x + e` everywhere, and the step slot as in (b).
- **Codegen, formatter, sema:** as (b), plus the new statement everywhere it can appear; `rae
  format` gains a rule (does it rewrite `x = x + 1` to `x += 1`, or not).
- **Programs touched:** none must change; a style question opens for thousands of `x = x + 1`.
- **Fit:** a second way to write every update (`x = x + 1` and `x += 1`), which Rae has avoided
  ("one obvious way"). Not needed for the loop itself if (b) is taken.

### (d) A stepping form over a range

`loop var degrees: Int in range(from: first, to: last, step: 5) { }`

- **Parser:** none if `range` is an ordinary function returning a list (`loop ... in` exists);
  the compiler would then build a list each loop. To cost nothing, the compiler recognizes
  `range(...)` in a loop header and emits a C `for` (a special case in sema and codegen).
- **Programs touched:** none.
- **Fit:** reads well for numbers (examples 1, 2), but a Float range needs rules for its end
  (inclusive? drift?), it does not walk a chain (example 4), and recognizing one function by name
  is the kind of special case Rae avoids. `to` inclusive or exclusive is one more thing to learn.

## Recommendation

**(b), with the error of (a).** The step slot becomes a statement slot that takes `++i`, `--i`,
or an assignment; anything else is an error that says so ("a loop's step is ++i, --i or an
assignment such as i = i + 5"). It is the smallest change that makes the obvious spelling work,
adds no operator and no special function, closes today's silent case (a bare call as the step),
and touches no existing program. (a) alone is the conservative choice if the language should keep
exactly one way to count; (c) and (d) add more than the problem needs.
