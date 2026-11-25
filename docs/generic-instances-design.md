# Generic instances: type-checked before C

A design, with measurements. Nothing of it is in the compiler yet. The numbers were measured on 0.4.95
(2026-11-25) with a prototype (`docs/generic-instances-prototype.patch`, against `c0b8197d7`), as CPU time, the
best of five runs on a busy machine.

**Recommendation: do it, in the nine steps of section 9.** The prototype, a few hours of rough work, already emits the C of
98 % of the instances from a type-checked clone; with it 1330 of 1359 test cases pass and the C of examples 106 and
136 compiles without an error. The 29 cases that fail have about six causes. The cost is 1.2 to 2.8 s of analysis in
our biggest programs, and it makes a pass of the backend unnecessary that takes 1.7 to 3.0 s in the same programs.

## 1. What happens today

A generic function (`func same(T: type, value: view T) ret T`) is a template. For each set of type arguments the
program uses, its C is emitted: an instance. `emit_specialized_function` (`c_backend.c`) walks the TEMPLATE's body
and keeps `generic_params` (T) and `generic_args` (String) beside it. The nodes of the body have no types, because
one node cannot hold the types of two instances.

What sema (`sema.c`) does with generic code:

| what | where | what is checked |
|---|---|---|
| a template | `sema_analyze_module`: "marked as processed but not analyzed" | names (`sema_check_template_names`), unsafe obligations by name, `break`/`continue`/`ret` by shape |
| an instance a method call or a resolved call makes | `specialize_decl` | its body is cloned and **only the first statement of it is analyzed**; the clone is not what is emitted |
| a generic call sema leaves to the backend | `resolve_function_overload` answers nothing | its arguments; since 0.4.95 the type of its result where it lands (`sema_value_type`) |
| a spawned generic | `sema_spawn_generic_call` | a wrapper function of concrete types, which calls the generic one |
| `loop ... in fields(value)` | `reflect_expand_field_loop` in plain code, `reflect_instantiate_body` at emission in a template | unrolled per struct; in a template after sema, so not analyzed |
| moves and destructors | `sema_check_specialization_flow`, run by the backend per instance | by name, with the moves the emission recorded |

`clone_expr` / `clone_stmt` are not complete: a `match` statement, a destructuring, the elements of a list literal
and the operand of `own` are shared between the clone and the template, so analyzing one writes into the other.

What the backend then works out by itself, once more for each instance:

| question | code |
|---|---|
| the type of any expression | `infer_expr_type_ref` (520 lines, about 120 uses) |
| the same with T put in | `substitute_type_ref` (about 160 uses), 89 times as `if (ctx->generic_params && ctx->generic_args)` |
| which instance a call means | `emit_call_expr`, in five ways tried in turn: the arguments, the type that is expected, the names of the type parameters, the receiver, the first candidate |
| which instances exist | `c_discovery.c` (890 lines), run twice, and again while emitting (late ones) |
| what is expected here | `has_expected_type` / `expected_type`, 126 references |
| whether a value moved | `c_record_moved_local`, `c_forget_rejected_moves` |
| type arguments written as values | `hoist_type_arg_if_present`, `try_as_type_arg` |
| slots, defaults, field loops | `c_raw_slot_type`, `c_raw_slot_field_type`, `c_type_parameter_default`, `reflect_instantiate_body` |

Each of these answers is right in the common cases and has been corrected, one combination at a time, in most of the
fixes of 0.4.59 to 0.4.95: optionals as a T, enums beside Int, Arrays, fixed-width integers, moves, tasks, text and
`is` of each. The last one found while measuring for this document, in plain code: in
`check(ok: same(value: box.held) is other)` the Bool that the argument wants is taken as what `same` answers, so
`same` is made for Bool.

## 2. What is proposed

For each instance: a full clone of the template's body with T written out, analyzed by sema exactly as a plain
function is, and emitted exactly as a plain function is.

1. **The clone is complete.** Every node is new. T is replaced in every written type (`let x: List(T)`), in every type
   argument (`createList(T, cap: 4)`, `sizeof(Entry(T))`) and as a type's name (`T.default()`).
2. **Sema decides the instance of every call**, in plain functions and in instances: the call's `decl_link` is the
   instance and its `resolved_type` is concrete. An instance is made once, in a registry keyed by the template and the
   interned types; making one puts it on a worklist. Sema ends when the worklist is empty: then every instance of the
   program is known, with its type arguments.
3. **An instance is checked in full**: types, names and opens (of the template's file), frozen `let`s, `own` and
   moves, returns, `unsafe`. The move check runs on it as on any function.
4. **An error names the instance.** Once per source position, with what it was made for:
   `lib/Pairs.rae:12:9: cannot convert Note to String; they are different types (in wrong(T: Note), from Main.rae:13)`.
   Today the same error appears several times and says nothing of the instance. An error that every instance has is
   reported for the first only.
5. **A branch that cannot be reached for a type is not checked for it**: `if typeName(value) is "Int"` and a field loop
   over a struct without fields are decided on the clone, as plain code does now (`reflect_constant_condition`).
6. **The backend emits a list of plain functions.** It reads `resolved_type` and `decl_link` and has no type parameters.

Not proposed: a separate typed tree (docs/tast-monomorphisation-plan.md, an older plan, wanted one). The AST with its
`resolved_type` and `decl_link` is enough, and sema fills 95 to 97 % of it today (section 4).

## 3. Measured: how many, how much

| | 106 mobile UI | 121 UI editor | 136 walker |
|---|---|---|---|
| instances with a body | 3 943 | 5 286 | 5 044 |
| (registered, with externs) | 5 493 | 6 783 | 7 130 |
| nodes cloned | 166 225 | 257 237 | 201 506 |
| nodes of a body, on average | 42 | 49 | 40 |
| cloning | 7 ms | 11 ms | 9 ms |
| sema of the program today | 1.02 s | 1.04 s | 4.04 s |
| backend today | 4.30 s | 6.85 s | 12.20 s |
| ... of it, discovery of instances and types | 1.73 s | 2.52 s | 2.96 s |
| ... of it, emitting the instances | 0.46 s | 0.93 s | 0.65 s |
| ... of it, `infer_expr_type_ref` (all / in instances) | 0.45 / 0.26 s | 0.72 / 0.46 s | 0.78 / 0.36 s |
| **analysis of the instances** (prototype) | **1.23 s** | **2.83 s** | **2.16 s** |
| backend with the prototype, discovery still there | 5.19 s | 9.20 s | 11.90 s |
| the arena today / with the prototype | 596 / 628 MB | 828 / 864 MB | 877 / 916 MB |

(The 2 GB the arena has are enough. The prototype's clones are freed with it, as everything is.)

The analysis costs 0.3 to 0.5 ms per instance, 7 to 11 µs a node. A profile of 121 with the prototype shows, on top,
linear scans that a name or a hash answers: `edge_by_name` (the may-wait walk) and `type_refs_equal` in
`register_function_specialization` (every instance, per registration), by far the two largest, then
`generic_index_any`, `sema_call_returns_reference` (every declaration, per `let`) and
`type_registry_find_specialization` (every instance, per call). The first two are in today's compiler as well, and
worth fixing for themselves.

**Estimated after all steps**: discovery goes (its type collection stays, about 0.3 s), the analysis stays at 1.2 to
2.8 s before those scans are fixed. For 106 that is 4.30 - 1.4 + 1.2 = about 4.1 s; for 121 about 7.4 s; for 136 about
11.6 s. So the same compile time within 10 %, and less with the scans fixed. That is an estimate; step 7 measures it.

## 4. Measured: can sema type them?

Expressions that have a concrete type after the analysis (a call that is a statement is not counted):

| | 106 | 121 | 136 |
|---|---|---|---|
| plain functions, today | 111 814 of 117 250 (95.4 %) | 144 098 of 150 730 (95.6 %) | 237 261 of 247 359 (95.9 %) |
| instances, prototype | 105 919 of 109 065 (97.1 %) | 186 249 of 193 440 (96.3 %) | 128 410 of 131 963 (97.3 %) |

What has none are calls of generic functions (2 201 of 7 394 calls in the instances of 106), and in plain code also
names and fields below them. `sema_value_type` (0.4.95) answers those, and step 4 makes it the rule.

Instances that sema refuses: 18, 30 and 20. They are 18 places. **13 are code in `lib/` that breaks Rae's rules where
nothing checked it**, and 5 are branches for other types. (There will be a few more: these are the instances of three
programs.)

| rule | where |
|---|---|
| a `let` that is changed | `StringMap.remove`, `IntMap.remove` (`entry.occupied`), `Query.query2`, `without` (`matches`), `Tag.taggedEntities` (`out`) |
| a parameter without a mode | `Atomic.createAtomic`, `store`, `fetchAdd`, `fetchAddRelaxed`, `SpscRing.createSpscRing`, `push` |
| a name of a module that is not open | `SpscRing.count`, `pop` (`rae_ext_rae_atomic_load`) |
| a branch for other types (section 2, point 5) | `Atomic.loadRelaxed` of a Bool (`as`), `decodeComponent`, `encodeComponent`, `tableWidgetRows`, `decodedFieldIn` of a struct without fields |

## 5. Measured: can the backend emit them as plain functions?

The prototype emits the C of an instance from its analyzed clone, with no type parameters in the context, and from the
template when sema refuses it. Nothing else in the backend was changed.

- 106: 3 860 of 3 943 instances; the C compiles. 136: 4 957 of 5 044; the C compiles. 121: 5 054 of 5 286; 74 C errors,
  all "conflicting types" (below).
- All cases: 1330 of 1359 with a verdict pass. The 29 others:

| cause | cases |
|---|---|
| the prototype of the function is still written from the template, and differs from the body (`const` of an enum) | 17 (1314, 1364, 1368, 1369, 1372, 1378, 1398, 1522, 884, 887, 900, 901, 1645, 1646, 1647, 1658, 1668) |
| a `view List` in text or `is` of a plain function is a pointer, of a template a value | 4 (889, 1276, 1654, 1655) |
| a leak, or memory that is not there: not looked into | 4 (1574, 1602, 1671, 1678) |
| `T.default()`, and the empty optional | 2 (1619, 1637) |
| the name of `List(Int)` in `typeName` | 1 (890) |
| a map entry's type is not collected | 1 (1651) |

- A spawn in a generic function (#83491520) resolves: its wrapper is the concrete one. It fails in the prototype only
  because the analysis runs after the spawn helpers were written.

## 6. What can be removed

With sema deciding instances and types: `c_discovery.c` (890 lines) but for its collection of types; `c_generic_call_type`,
`hoist_type_arg_if_present`, `try_as_type_arg`, `type_ref_mentions_params`, `c_type_parameter_default`,
`c_raw_slot_type`, `c_raw_slot_field_type`, `eff_base_name`, `infer_tr_from_resolved`; four of the five ways a call
finds its instance; `reflect_instantiate_body` and its walkers; `sema_check_specialization_flow`,
`c_record_moved_local`, `c_forget_rejected_moves`; the late specializations (`report_late_specializations`,
`emit_new_specialization_prototypes`); `generic_params` and `generic_args` of `CFuncContext` and their 89 guarded
substitutions; the wrappers of `sema_spawn_generic_call`. `infer_expr_type_ref` becomes a read of `resolved_type`, with
what the backend itself builds (hoisted temporaries) left. An estimated 2 000 to 3 000 lines, of 62 000; what counts is that each
question has one answer.

It stays: the struct shapes, the drops, copies, text and comparison of types (they are per type, not per function), the
twins, the loop versions.

## 7. Is it better than fixing bugs one by one?

The alternative was tried: over thirty versions of 0.4.59 to 0.4.95 in two days, each correct and each finding one or two more.
The combinations are types x sources x positions, some thousands, and the backend answers them in some hundred places.
Section 8's first run says where that stands: of 13 programs of 180 combinations, 2 compile. The others stop at three
bugs, two of them in plain code.

Against it: nine steps, each the size of one of those versions or a little more, and a time when two ways exist. What
keeps that small is that both ways are complete: the template way stays the default until the new one passes every
case, and one switch (`RAE_GENERIC_INSTANCES=0/1`) chooses. What it does not do: bugs that are not about types (drops
of a `ret`, the runtime) stay what they are.

## 8. A test matrix as a stage of the suite

`compiler/tools/typeMatrix.py` (`make -C compiler type-matrix`, since 0.4.96) writes two programs per element type; each
combination is a function that checks its own value:

- 15 types: Int, Float, Bool, UInt8, an enum, String, a struct with a List, one with a String, one of numbers,
  List(Int), List(String), a generic struct, a map, an Array of Strings, Any;
- 11 sources: a local, `own`, a call, a generic call, a method, a field, an element, a map's values, a match, a task's
  result, a literal;
- 17 positions: a `let`, a `var` set again, a field, a field set, a list literal, `add`, a map's `set`, an optional,
  arguments by view, own, copy and generic, text, `is`, `ret`, a defer, a spawn.

180 combinations a program (what Rae refuses is left out). In the `plain` program the type is written; in the `generic`
one every function has the type as `T`, the half this design is about. 5 400 in all. One build of each checks it all:
values (`WRONG <name>`), leaks (`RAE_MEM_STATS=1` at exit), memory errors (AddressSanitizer and UBSan).

**Running time**: 9 s of wall time on ten cores for the 30 programs (8 s of emitting, 46 s of C compiler and 8 s of
running, summed over the jobs): one `-O0` build with AddressSanitizer, against one runtime object for all.

`compiler/tools/type-matrix-known.txt` has what fails today, as `program function kind`. Such a function is left out of
its program and tried beside it; a line that names something that passes is STALE and fails the stage.

Its first run (two of 13 programs compiled) found, all fixed in 0.4.96: the expected type of an argument in a generic
call inside its `is` or text; a generic call of a task's result; an Any against a String or a number, and from a generic
call; `is` of a `view List`; a String moved into a task leaked; `make() is label` leaked; and in generic functions an
`opt T` nobody else names, and a generic call in a list literal. What is left is the spawn inside a generic function
(step 6): 390 combinations.

## 9. The steps

In QUEUE.md as `[generic instances n/9]`. Each leaves the suite green and is useful alone.

1. **The matrix** as a suite stage, with its known failures listed. Done (0.4.96).
2. **The full clone**: every node is copied, T written out (`compiler/src/ast_clone.c`). No behavior change. Done
   (0.4.97). A type that has no spelling as an expression (an optional) becomes a type argument of its call where it
   leads it, and stays `T` elsewhere, for the instance's scope to resolve. A node written out of a type parameter is
   marked (`is_written_out`), so sema knows `enumName(T, ...)` of a type that is no enum from a program's own mistake.
   `RAE_CHECK_CLONES=1` (on in the suite) compares each copy with its template.
3. **Instances analyzed in full** in `specialize_decl`, the 13 places in `lib/` fixed, errors once and with the
   instance's name, branches of other types left out.
4. **Sema decides the instance of every call**; a registry and a worklist; a check in the suite that its instances are
   those `c_discovery.c` finds.
5. **Emission from instances** behind `RAE_GENERIC_INSTANCES=1`, prototypes and helpers too; the 29 cases; the suite
   runs its cases both ways.
6. **On by default**, the switch kept for one version. A spawn in a generic function works (#83491520). The waits, the
   realtime check and the twins read the instances instead of following calls by name.
7. **Discovery removed**; the linear scans of section 3 replaced; the compile time measured against this document.
8. **The template way removed**: what section 6 lists, and the switch.
9. **`infer_expr_type_ref` reads `resolved_type`**; the expected type no longer chooses instances.

`a defer block is not checked` is done (0.4.90). `a spawn inside a generic function` (#83491520) is step 6: fixed
before, it would be work on the template way that step 8 removes.
