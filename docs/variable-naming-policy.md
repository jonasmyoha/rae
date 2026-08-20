# Opinionated variable naming (short-name policy) — design (#556)

Rae already hard-caps files at 1000 lines and documents camelCase/PascalCase.
This proposes a matching opinion on *variable names*: a name should carry
meaning, so **single-letter names** (and later, some two-letter abbreviations)
outside a small allowlist are flagged. Worse in Rae than elsewhere, because a
parameter name is echoed at every call site (`syncUi(world: world, s: state)`
puts a meaningless label in front of a meaningful value).

This document is the investigation asked for in QUEUE #556. It does **not** turn
on enforcement — see "Why not a hard error today".

## The blast radius (measured)

`grep` over `lib/` + `examples/` for `let|var|const <name>:`:

- **1606** single-letter local bindings. Top offenders: `i`(336), `n`(192),
  `r`(113), `a`(93), `s`(90), `t`(88), `e`(82), `v`(65), `h`(58), `p`(57),
  `c`(49), `k`(42) … plus `x`(24), `y`(23), `z`(16), `w`(24).
- **1168** two-letter local bindings.
- Pervasive single-letter **parameters** in maths/graphics: `x,y,z,w` (vectors),
  `a,b` (binary ops), `u,v` (texture coords), `r,g,b,a` (color).

A hard error flipped on today breaks essentially the whole tree and every
foreign/C binding. This is the same reason the parser's PascalCase/camelCase
checks were **removed** and turned into no-ops (`parser.c` ~1892,
`docs/naming-conventions.md` "Enforcement"): *"A hard parse error on an
unconventional name would break generated code, bindings to foreign/C APIs,
mechanical migrations, and intentionally unusual names."* The documented intent
there is **optional compiler/linter warnings, never compilation errors**.

## Why not a hard error today

1. **Blast radius** above — thousands of call sites, no migration done.
2. **Existing policy** is explicitly warnings-first for naming (above).
3. **Direct conflict with CLAUDE.md**, which lists `x, y, dt` and loop `i, j`
   as *"genuinely conventional maths … those names ARE the meaning"*. A blanket
   ban contradicts a rule the codebase is written to.
4. **No warning framework exists yet** (`docs/naming-conventions.md`; QUEUE
   #241). There is nowhere to emit a non-fatal diagnostic; only `diag_error`,
   which fails the build.
5. **The single-letter-`t` problem is semantic, not lexical.** `t` for *time*
   is bad; `t` for a parametric position in `[0,1]` is idiomatic. The compiler
   cannot tell them apart, so any rule that bans `t` also bans the good `t`.

The conclusion is not "don't do it" — it is "do it as a warning, behind the
warning framework, with a scoped allowlist, and let a project opt into
error-severity once it has swept its own code."

## Proposed rule

Scope: **local bindings** (`let`/`var`/`const`) and **function parameters**.
NOT struct/enum field names (a `Vec3 { x, y, z }` field is the meaning) and NOT
`extern`/C-binding names (exempt, like the case conventions).

Phase 1 — **single-letter names**, flagged unless allowlisted:

- **Loop induction variables** `i, j, k, l, m, n` — allowed, but ONLY when the
  name is the loop's own binding (`loop i: Int = …`, `loop i in …`). The same
  letter as a plain `let i` *outside* a loop header is flagged. This is exactly
  the user's "only loops should allow i j k l m n".
- **Conventional maths / graphics**, allowed anywhere (these letters ARE the
  meaning, and CLAUDE.md protects them):
  - coordinates/vectors `x, y, z, w`
  - texture coords `u, v`
  - color channels `r, g, b, a`
  - binary operands `a, b` (already covered by color `a, b`; add nothing)
- Everything else single-letter — `t, s, c, e, p, h, q, d, o, f, g` … — is
  flagged. This is what catches `t` for time, `s` for state, `c` for count.

Phase 2 (later, separate decision) — **two-letter abbreviations**. Much broader
and much noisier: it catches `dt`(deltaTime), `id`, `ok`, `hi`/`lo`, `tl`/`br`,
`ax`/`ay`. Many are defensible. Recommendation: do **not** ship this in phase 1;
if wanted, ship it as a distinct, lower-severity note with its own allowlist
(`dt`, `id`, `ok`, `pi`, `tau`, `ui`, `os`, `io`, `ms`, `hp`, `mp`, `nx/ny/nz`).
The user's own "possibly even dt" uncertainty is the signal to defer this.

## The allowlist, concretely

```
loop-only : i j k l m n           (only as the loop's induction binding)
always    : x y z w   u v   r g b a
```

Anything else that is exactly one letter → diagnostic. The allowlist lives in
one table in the checker so it is trivially auditable and tunable.

## Opt-out mechanism (the pragma question)

The user asked about per-line / per-file / per-project opt-out. Rae has **no
pragma/attribute syntax today**, and deliberately ships **no override directive**
for the 1000-line cap (`lexer.c` ~505: *"There is intentionally NO override
directive"*). So adding per-line escape hatches is a philosophy shift, not just
a feature. Options, least- to most-invasive:

1. **Severity, per project (RECOMMENDED first).** The check is a **warning** by
   default. A project opts into hard errors when it has swept its own code, via
   a compiler flag / project config (mirroring the existing `MODE=CI_STRICT`
   build gate): `--name-policy=off|warn|error` (default `warn`). No new syntax;
   no per-line noise; reversible. While it is a warning, no opt-out is even
   needed — warnings don't block builds.
2. **Per-line comment marker**, only if/when a project runs at `error` severity
   and hits a legitimate exception. Reuse comments (no new token):
   `let t: Float = param(...)  # rae:allow-name` on the same line. Comments are
   already preserved as tokens (`TOK_COMMENT`), so the checker can look for a
   trailing marker on the binding's line. Cheap, but it re-introduces exactly
   the override directive the maintainers avoided for the file cap — so gate it
   behind a real need.
3. **Per-file marker** — a top-of-file `# rae:allow-short-names`. Discouraged: a
   whole-file blanket defeats the point and rots (people forget to remove it).
4. **Per-project pragma** — the user's own "not sure". Recommendation: express
   project-level intent through **severity (#1)**, not a blanket allow. A
   project that wants short names everywhere sets `--name-policy=off`; one that
   wants them nowhere sets `error`. That is the per-project knob, without a new
   pragma concept.

**Recommendation:** ship #1 only (severity flag, default `warn`). Add #2 (the
per-line comment escape) *only* once someone runs at `error` and needs it. Skip
#3/#4.

## Rollout

1. **Warning framework (#241, prerequisite).** A `diag_warn` path that prints,
   counts, and does NOT set `module->had_error`, plus a severity knob. Nothing
   here can land without it.
2. **Single-letter checker (phase 1)** in sema, over let/var/param bindings,
   using the allowlist + loop-induction context. Default `warn`.
3. **Measure & sweep** at `warn`: the ~1600 hits are the migration list. Sweep
   `lib/`/`examples/` (rename `t→time`, `s→state`, `c→count`, …) as separate,
   reviewable commits — do NOT mass-rename in one.
4. **Opt-in `error`** per project once its own code is clean (CI can pin it).
5. **Two-letter (phase 2)** as a later, separate proposal if still wanted.

## Open decisions for the human

1. **Two-letter names** — defer (recommended) or include now? (`dt`, `id`, `ok`
   would all be flagged if included.)
2. **Maths allowlist** — is `x y z w u v r g b a` the right always-allowed set,
   or tighter/looser? Any others (`n` for normal/count, `t` for parametric)?
3. **Opt-out** — accept "severity only, no per-line pragma" (recommended), or is
   a per-line comment escape wanted from day one?
4. **Parameters** — enforce on parameters too (recommended, since the call-site
   echo is the whole argument), or bindings only at first?

Once these are settled, the implementation is small and gated on #241.
