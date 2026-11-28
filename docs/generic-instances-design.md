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

**Measured after step 7's first part (0.4.130)**, CPU time of `rae build --emit-c` (user + system, the lowest of
three runs on a loaded machine), against the compiler as it stood before step 4 measured above (sema + backend):

| | 106 mobile UI | 121 UI editor | 136 walker |
|---|---|---|---|
| before step 1 (sema + backend in the table above) | 5.3 s | 7.9 s | 16.2 s |
| 0.4.127 (instances from clones, behind the switch) | 8.3 s | 13.2 s | 24.5 s |
| 0.4.128 (on by default, `edge_by_name` a lookup) | 4.5 s | 6.8 s | 12.3 s |
| 0.4.130 (the scans of this section lookups) | 4.2 s | 5.7 s | 9.2 s |
| 0.4.132 (sema's instances alone, discovery collects types) | 3.65 s | 4.9 s | 8.1 s |

The four scans: `edge_by_name` (0.4.128), `type_refs_equal` in `register_function_specialization` (now an index from
a template to its instances, in their order), `sema_call_returns_reference` (an index of the module's functions by
name; its answer does not depend on their order) and `type_registry_find_specialization` (no longer in the
profile). `generic_index_any` stays a chain per name: a finer bucket that keeps `type_refs_equal`'s answers (the
name, the marks and the number of arguments) was tried and changed nothing, every `List` having one argument. Of
121's build, discovery is now about 6 % (`discover_specializations_stmt_impl`), the type collection about 15 %
(`collect_type_refs_module`, half of it `register_generic_type` and `generic_index_any`), the analysis of the
instances about 13 % (`sema_drain_instances`).

**Estimated after all steps** (written before step 4): discovery goes (its type collection stays, about 0.3 s), the analysis stays at 1.2 to
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
- 12 sources: a local, `own`, a call, a generic call, a method, a field, an element, a map's values, a match, a task's
  result, a literal, one of a function's several results;
- 17 positions: a `let`, a `var` set again, a field, a field set, a list literal, `add`, a map's `set`, an optional,
  arguments by view, own, copy and generic, text, `is`, `ret`, a defer, a spawn.

180 combinations a program (what Rae refuses is left out). In the `plain` program the type is written; in the `generic`
one every function has the type as `T`, the half this design is about. 5 910 in all. One build of each checks it all:
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
   instance's name, branches of other types left out. Done (0.4.98). Sema makes only a few hundred instances of a
   program (in 106, 310, mostly `componentSet` and buffer externs); the backend makes the rest (3 943). So both are
   analyzed: sema's where they are made, the backend's just before their C is written
   (`sema_check_backend_instance`; `RAE_CHECK_INSTANCES=0` turns that off). Each is analyzed in a scope that sees the
   module but not the locals of the call that made it, and in it the functions declared next to its type arguments
   are open (a hook such as `walkerGameStart(game: mod BoxGame, ...)` of lib/app3d's `runWalkerGame`). A `match let`
   of a template is rewritten before anything is cloned or emitted from it. A field loop over a struct without fields
   has no pass in an instance, and there a Bool converts to and from a number with `as` (Atomic(Bool)). Besides the
   13 places in `lib/`, the examples 134 and 135 changed a `let`. A destructuring `let a: x, let b: y = f()` fails in
   plain functions too: it is a gap of its own. What sema finds inside an instance is not registered for the C (the
   backend finds what it emits until steps 4 and 5). The cost, CPU time of `rae build --emit-c`: 106 5.5 s to 6.5 s,
   121 7.9 s to 10.0 s, 136 15.7 s to 19.1 s (the analysis section 3 measured); steps 7 and 8 take back more.
4. **Sema decides the instance of every call**; a registry and a worklist; a check in the suite that its instances are
   those `c_discovery.c` finds. First part done (0.4.105): the registry is a hash of the template and the interned
   types (`type.c`). After each call is analyzed, `sema_decide_instance` (`sema_instances.inc`) finds its template and
   type arguments, written or inferred from every value whose type sema knows, and keeps the instance in the call's
   `instance`. A new instance goes on a worklist and is analyzed after the function that made it, until no new one
   comes. Calls nobody writes have their instances too: each concrete type whose template has a generic `drop` gets
   that drop, and a list literal gets `createList` and `add`. A method's receiver must be its first parameter's
   type (a map's `set` does not take V from the value alone), and a plain call whose first value is an Array finds
   the Array method of that capacity. `RAE_CHECK_INSTANCES=1` prints what only the backend or only sema has
   (`c_instance_check.c`); `RAE_SEMA_INSTANCES=0` turns sema's deciding off; `RAE_DEBUG_INSTANCES=1` names each
   call it decides or cannot. Of the 1097 cases that build, 668 have the same instances both ways. `decl_link`
   stays what the backend resolves: pointed at the instance, the backend writes the call from the instance's
   parameters and the function from the template's, and they differ (`view T` of an Int is the Int, `view Int`
   is a pointer); so it moves to the instance with step 5. An instance is named from its template
   (`rae_mangle_specialized_function`). Second part (0.4.106): every case and the examples 106, 121 and 136 have
   the same instances both ways (106: 3 948). Sema also reads a type argument written as a call (`List(Int)`,
   `Task(Int)`), by its parameter's name (`T: T`), or as the qualifier (`Float.createList(cap: 4)`, `Query.query2`);
   it ranks templates that fit as the resolver does (one with `this` for a method call, then the program's own, and
   the argument names), and gives a generic struct's list-field defaults and every field default their literal
   instances. Instances only sema has are counted, not differences: sema decides some the C never needs (the
   drop of a type that is only declared, `List(SpatialRect)`). Emitting them was tried: sema spells some number
   types by their C name (`uint64_t`), so one C function was registered twice with two parameter types; that
   spelling is fixed with step 5. An instance only the backend has is a difference only where the C calls it:
   discovery registers some that nothing calls (`add` of `Stack(UInt8)`), which step 7 removes. The drops are
   made in type-name order, so the C is the same on every run. Third part (0.4.107): an instance's type arguments are spelled as
   Rae types (`UInt8`, not the TypeInfo's C name `uint8_t`), so sema decides inside spawn wrappers and spawned
   calls too, and the backend also emits the instances sema decided that discovery did not find (no "sema only" is
   left); where both have one, the backend's spelling stays (its C of a `view` of an enum is `const`).
   A generic call has its instance's result as its type, an optional one too since 0.4.108 (the places in lib/ that
   used one unchecked were fixed: the rule for a plain call holds for a generic one), and `sema_value_type` reads a
   call's type instead of working it out again. The suite builds the cases with `RAE_CHECK_INSTANCES=error`, which
   fails on a difference.
5. **Emission from instances** behind `RAE_GENERIC_INSTANCES=1`, prototypes and helpers too; the 29 cases; the suite
   runs its cases both ways. First part done (0.4.125): with the switch, `emit_specialized_function` writes an
   instance's prototype and body from its analyzed clone (`sema_backend_instance_decl`: the registry's clone, made and
   analyzed when sema has none yet) as a plain function, with no type parameters in its context, and skips the
   template-only steps (`reflect_instantiate_body`, `sema_check_specialization_flow`: sema checked the clone's moves).
   The three loops that wrote instance prototypes are one function (`emit_specialization_prototype`), so a prototype
   and its body come from the same place (that was the prototype's 17 "conflicting types"). `enumFromName`,
   `enumName`, `enumMembers` and `enumCount` take a type written out as a call (`List(Int)`). Off, nothing changes.
   On: 1395 of 1415 cases pass; the type matrix has one failing type (a StringMap of an Array) where the template
   way has 405 known failures, the spawns in a generic function among them; and `Array(T, cap: 2)` in a generic
   function compiles. The stage `tools/generic-instances-stage.sh` (pre-suite) runs the 93 cases that declare a
   generic function and the listed ones with the switch against `tools/generic-instances-known.txt`, and the matrix
   against `tools/type-matrix-known-instances.txt`; both lists only get shorter. The 20 listed cases, by cause: an
   optional type parameter stays its name `T` in the clone, which the backend cannot resolve without type
   parameters (8: `sizeof(T)`, `T.default()`, `List.create(V, ...)`, a `view` of an optional); a map's entry type
   is not collected for an instance (4); a type the backend spells empty, `rae_` (4); a `view List` in text or `is`
   (2); leaks (1574, 1602). They are the queue line `[generic instances 5b/9: the 20 listed cases]`. Second part
   (0.4.126), ten of them and the matrix: a type written out as a call with no declaration (`Task(Int)`) or with a
   value argument (`Array(Int, cap: 2)`) is a type argument (`try_as_type_arg` reads a node the clone wrote out,
   and a call's own type arguments, where the clone puts an optional `V` of `Entry(V)`); a name the clone left
   (an optional type parameter) is read from the clone's `specialization_args` (`c_clone_type_parameter`);
   `sizeof(Entry(V))` reads its argument as a type first; `Pair(Int).default()` of a written-out T; an argument
   the backend read off a `mod T` place is keyed by its referenced type (it made a second instance with types it
   could not spell, `rae_`); and `var zero: T` without a value keeps the template's meaning in the clone, all
   zeros (`zero_init`, set by the cloner), not the struct's declared defaults. The type matrix passes with the
   switch, its list is empty. Ten cases are left, the queue line `[generic instances 5c/9: the 10 listed cases]`:
   a `view T` of an optional T (887, 1645, 1668: in the template a pointer to the optional, in the clone `view`
   and `opt` of the payload, the same as a written `opt view X`, so the type needs a mark of its own and the
   body's reads with it; a mark on the prototype alone was tried, the body still read it as an optional view),
   an Array's elements (1671, 1678), a `view List` in text or `is` (889, 1676), a map of an optional aborting at
   run time (1654), and two leaks (1574, 1602). Third part (0.4.127), the rest: with the switch every case passes
   (all 1415, not only the 93 the stage runs) and both lists are empty. A `view T` / `mod T` of a T bound to an
   optional is marked (`AstTypeRef.is_optional_box`, set by `substitute_type_ref` and the cloner): a pointer to the
   optional in its prototype, and a read of it is `(*name)` of the optional's type, still a reference for an `own`
   argument (it is copied). A `let x: T` of the template keeps its meaning in the clone
   (`let_stmt.type_was_parameter`): a raw slot read is the slot's bits, with no pool take, no clearing of a String's
   ownership (List.pop moves it out) and no drop at the end of the scope (`local_slot_of_parameter`); a raw slot
   read written with a concrete type in plain code still owns what it reads (lib/perf's name box). The clone's
   hidden locals are renamed (`c_rename_hidden_names` on instances with the switch). And three fixes for plain
   code that the clones found: `equals()` of a `view List` name reads the list (it was its pointer), `Array.set`
   moves its `own` value in (`this[index] = own value`; as `= value` the clone stored the bits and then dropped
   the value), and `ret none` of an optional reference flushes the function's String pool mark.
6. **On by default**, the switch kept for one version. A spawn in a generic function works (#83491520). The waits, the
   realtime check and the twins read the instances instead of following calls by name. First part done (0.4.128):
   instances are written from their clones by default, and `RAE_GENERIC_INSTANCES=0` is the way back, which the
   pre-suite stage now runs (the cases that declare a generic function, against `generic-instances-known.txt`, and
   the type matrix against `type-matrix-known-template.txt`, the template's 405 known failures); the main type
   matrix list is empty. A spawn inside a generic function works (case 1709, on the ThreadSanitizer list); with
   `RAE_GENERIC_INSTANCES=0` it is still an error. The may-wait walk's calls resolved by name (`edge_by_name`, the
   largest item of a profile of 121, and what the realtime check builds on) look the name up in an index of the
   merged module, made once per walk, instead of scanning every declaration; the C of 106, 121 and 136 is the same
   as 0.4.127 with the switch, apart from the shader text the two checkouts embed. Compile CPU time (`rae build
   --emit-c`, with the full suite running beside it): 106 8.3 s to 4.5 s, 121 13.2 s to 6.8 s, 136 24.5 s to
   12.3 s; with the switch alone, before the index, the two ways cost the same (106 8.2 s, 121 13.3 s, 136 24 s).
   Every example (86) and check (67) builds both ways, the examples' C compiles, wasm_smoke and wasm_webgpu_smoke
   pass, and the visual gate of 106 and 136 passes. Second part (0.4.129): the walks read instances.
   The may-wait walk (and the realtime check, which builds on it) takes a generic call's target from the instance
   sema decided (`edge_to_instance`: the template with the instance's types, the node a written-out call reaches),
   and walks an instance node's analyzed clone instead of the template (`sema_find_instance_decl`, which makes
   nothing), so the calls in it carry their instances too. The twins ask the same of a call
   (`may_wait_call_reaches_sleep`: a generic call's own instance node; through `decl_link` alone it was the
   template, which has no node, so a generic call was never taken to sleep). Calls resolved conservatively:
   106 3 + 3344 in lib/ to 0, 121 5543 + 5268 to 0, 136 8 + 3839 to 0. On the way a bug of the field-loop
   rewrite was found: `reflect_expand_field_loop` turned a loop into an `if` in place without clearing the
   `if`'s fields, which share the loop's memory, so `is_task_scope` could read as true and the walk counted a
   task join that is not there (every "task join" of 106, 121 and 136 was one). With both, the may-wait sets
   are smaller (121: 200 to 23 in the program, 301 to 99 in lib/), the resumable twins and the C of 106, 121
   and 136 are the same, and `--report-realtime` lists a function's trusted C in the order the walk meets it
   (case 1407).
7. **Discovery removed**; the linear scans of section 3 replaced; the compile time measured against this document.
   First part done (0.4.130): the scans (section 3, measured there). Second part (0.4.131), behind
   `RAE_DISCOVERY_INSTANCES=0`: sema's instances are registered first, in the order sema made them (the registry
   lists them newest first; reversed, an error each instance reports while it is written comes in the program's
   order, case 1397), and discovery's walk only collects types. With it every case passes but 1648, and 106,
   121 and 136 have the same functions with the same bodies as with discovery (only their order differs), in 12 to
   15 % less CPU (106 4.2 to 3.7 s, 121 5.9 to 5.1 s, 136 9.3 to 7.9 s). It is not the default because the call
   emission still resolves a call by itself: in 1648 (a program's own `add` beside List's) it registers an
   instance sema never made (`add` of `Stack(UInt8)`, while writing lib's `jsonScanString`, which then calls the
   other one), and with discovery off no walk collects that instance's types, so its C does not compile. The fix
   is the queue line `[generic instances 7c/9: the call emission reads instances]`: a generic call's target from
   sema's `instance`. On the way, the late-specialization report (`RAE_LATE_SPECIALIZATIONS`) no longer counts an
   instance that is the C function of an earlier one written another way (`Char32` / `Char`), nor one the body
   does not call. Third part (0.4.132): the stray instance of 1648 was not the call emission's. A list literal
   (`let bytes: List(UInt8) = {}`) found its `createList` and `add` as the last generic function of that name in
   the program, which in 1648 is the program's own `add(this: mod Stack(T), ...)`; it now takes the one over a
   List (`c_list_literal_function`, the emission's and discovery's search alike), so a non-empty literal can no
   longer call the wrong `add` either. With that the call emission registers nothing sema did not make (the
   suite builds with `RAE_LATE_SPECIALIZATIONS=error`), and sema's instances alone are the default;
   `RAE_DISCOVERY_INSTANCES=1` is the way back for one version. 106, 121 and 136 have the same functions with the
   same bodies as 0.4.130, in the table above. Taking a call's target from its `instance` in `emit_call_expr`
   was not needed for it, and is left to step 9 (the backend reads what sema found).
8. **The template way removed**: what section 6 lists, and the switch. First part done (0.4.133, 0.4.134): no
   case and none of 106, 121 or 136 emitted an instance with a body and no clone, nor walked one in discovery,
   so both template paths went: `RAE_GENERIC_INSTANCES` and `RAE_DISCOVERY_INSTANCES` with their pre-suite
   stage and lists, the backend's `reflect_instantiate_body`, `sema_check_specialization_flow` and the lifecycle
   flow's template mode (the recorded moves), the template's spawn error, and discovery's walk of a template with
   substitution (it walks the analyzed clone as a plain function, registering a `let`'s written-out type itself:
   the substituted one was registered only under type parameters, so a `let pair: Pair(T)` in a generic body
   lost its drop, cases 1619, 1676, 1691). The C of 106, 121 and 136 has the same functions and typedefs; CPU 3.6,
   4.9 and 8.0 s. Left, the queue line `[generic instances 8b/9: the backend's type parameters removed]`:
   `CFuncContext`'s `generic_params` / `generic_args` (still set for a generic struct's fields, a callee's
   parameter types at a call and a prototype with no clone) and their guarded substitutions (about 400
   mentions in `c_*.c`), the late-specialization report and `emit_new_specialization_prototypes`, the helpers
   that guess, and the wrappers of `sema_spawn_generic_call`. Then (0.4.135): no case, example or check
   registered an instance while the C was written but the 39 that were an alias's spelling of one already there
   (`Float32` for `Float`, `Char32` for `Char`; `register_function_specialization` now takes two spellings of one
   primitive as one instance), so `RAE_LATE_SPECIALIZATIONS` and its report are gone and such an instance is an
   internal error; a prototype with no clone is no longer written; and nothing sets `CFuncContext`'s
   `generic_params` / `generic_args` any more: a generic struct's field types are substituted once where its C
   struct is written, and whether a callee's `view T` takes an optional is read from the callee's type parameters
   and the call's arguments. Every guarded substitution on them is dead code now; removing the fields and the
   guards, with the helpers that guess and the spawn wrappers, is the queue line
   `[generic instances 8c/9: the dead type-parameter code removed]`. Then (0.4.136): the two fields are gone from `CFuncContext`
   with every use: the guarded substitutions, the mangler arguments (NULL), and the helpers that acted only
   through them (`c_type_parameter_default`, `c_record_moved_local`, `respecialize_overload` and its
   no-overload report, the loops over the parameters in `c_type_parameter_is_optional`, the specialized name of a
   results struct). The C of 106, 121 and 136 is byte-identical. Left, the queue line
   `[generic instances 8d/9: the guessing helpers, the prototype buffering and the spawn wrappers removed]`. Then (0.4.137): a probe at
   each site, over every case and 106/121/136, logged what reached it, and each site was turned off alone and the
   C compared. `c_generic_call_type` and the `ret Box(V)` branch of `infer_tr_from_resolved` were never reached; its
   `ret opt T` branch and the generic method's `opt`/`view` slot branch were reached but changed no C. All four are
   removed, and so is the prototype buffering: the bodies are written straight out, and an instance registered from
   the task drops on is reported at the end (the build fails, as before). The identity helpers
   `c_concrete_type_ref` and `c_list_element_type` and five unused `ctx` parameters went with them. The C of every
   case and of 106/121/136 is byte-identical. Still live, so left to step 9: `hoist_type_arg_if_present` (without
   it nearly every program's C changes, and 1103 do not build: a written type argument, `createList(Int, ...)`,
   reaches the backend in the arguments), the other three `infer_tr_from_resolved` branches (503, 93 and 15
   programs change), and the spawn wrappers (a spawn of a generic function runs through one: spawning the
   instance itself needs a thunk per instance).
9. **`infer_expr_type_ref` reads `resolved_type`**; the expected type no longer chooses instances. First part (0.4.139):
   `RAE_CHECK_TYPES=1` compares each answer of `infer_expr_type_ref` with the type sema gave the expression and
   prints the counts per expression kind at exit (`RAE_CHECK_TYPES_SAMPLES=<file>` lists each disagreement);
   `compiler/tools/check-types.sh` sums them over every case and 106/121/136 in about 2.5 minutes. Of 240 million
   answers, 238.5 million agree. Sema now types an enum case read (`Kind.small` is a `Kind`), which made
   `nameOf(value: Kind.small)` call the `Kind` overload instead of the `Other` one (case 1656's expected output had
   that wrong result). What is left, in the order the queue takes it:
   - a module-level const has no type in sema (1.06 million name reads). Typing it where it is read changes the C
     of 62 programs: a float literal beside `toFloat(<const>)` becomes f32 (the rule), and a sum of `UInt64`
     consts is overflow-checked as `Int` in its outer additions only, because the backend says `Int` where sema
     says `UInt64` (12 000 binaries differ that way). The backend's integer width goes first;
   - the backend has no type for a unary expression (0.31 million), a box (0.18 million), an `own` (0.04
     million), some binaries and index reads (0.03 million), where sema has one;
   - an `if let` alias: the backend says `opt view T` / `opt mod T`, sema `view T` / `mod T` (28 000);
   - then `infer_expr_type_ref` becomes a read of `resolved_type`, a missing type an internal error under
     `RAE_CHECK_TYPES=1` (on in the suite), and the expected type stops choosing instances.
   The type matrix's known-failures list is already empty (since 0.4.127). Then (0.4.140): the backend's type of an integer binary keeps
   its width (`UInt64` flags), so a literal beside it is that type and an unsigned multiply wraps instead of going
   through the `Int` overflow check (`lib/physics/geometry/HullContent.rae`'s hash); and sema types a module-level
   const where it is read. The C of 233 + 61 programs changed, each line by one of those rules (a float literal
   beside `toFloat(<const>)` is f32, an `Int` expression of consts is overflow-checked, a function that holds no
   String loses its string-pool mark). check-types.sh: binaries differ 1 (was 12 135), names with a type only in
   the backend 127 (was 1.06 million). Then (0.4.141): where the backend worked out no type and sema
   has one, `infer_expr_type_ref` answers sema's (`c_sema_type_of`) for a unary expression, a box, an `own` and a
   binary (`Float64`), but not for `view x` / `mod x` (the place itself, passed by address: typed, an argument
   lost its `&`) nor for an index of a `Buffer` (a write into its slot is raw; typed, it dropped the slot's old
   value first). And sema types the binding's name in an `if let` presence test as the optional it still is
   (it said the narrowed `view T`, the backend `opt view T`). The C of 106 and 121 is unchanged; 44 programs'
   C changed, each by a function or statement losing a string-pool mark it never needed, or `log(x++)` taking
   the typed path instead of an Any; 136's one change is the first kind. check-types.sh: 239.8 million agree,
   1 730 differ (was 30 221), 118 853 only in sema (was 690 869), 5 535 only in the backend. Then (0.4.142): a name or a field
   read the backend asks about before its local is declared (a scan of a whole body at its start) has sema's
   type, and such a name no longer falls back to a module-level value of the same name (`const source: String`
   in case 1378 typed a local `source: Int`): the fallback takes the global sema linked, and none for a name sema
   typed without a link. No program's C changed. check-types.sh counts a reference to an optional read through
   (`value: view T` with T an `opt Int`, whose C is `(*value)`) as agreeing with the backend's `opt Int`. Left:
   74 differ, all bindings the backend makes itself (`mainLoop app { }` moves the body into a function where
   `app` is a `mod App`); 5 535 typed only by the backend, nearly all a generic extern called with a written
   type argument (`rae_ext_rae_buf_get(V: Int, ...)`, 4 763 in `readFileBytes`), which sema leaves untyped, and
   the nodes of a `query(...)` loop the backend builds; and the two kinds left untyped on purpose, `view x` /
   `mod x` and an index of a `Buffer`. Then (0.4.143): sema moves a type written as a generic extern's first argument
   (`rae_ext_rae_buf_get(V: Int, buf:, index:)`) into the call's type arguments, as the backend's
   `hoist_type_arg_if_present` reads it, and types the call as its return with them put in (only for an extern:
   a resolve with type arguments makes the instance it finds, and trying it on every call reordered the instances
   of 1 118 programs). The argument code writes a `view x` / `mod x` argument as the place `x`
   (`c_without_reference_mark`), so it no longer reads "no type" as "a place"; and an assignment asks
   `c_is_buffer_slot` for a raw write. With both, `view x` / `mod x` and a `Buffer` index have sema's type. C
   changes: 1441's `7.5` into a `Buffer(Float)` slot is `7.5f`; 1096 and 608 pass the same pointer in another
   spelling (`(rae_Mod_String){ .ptr = &((*text.ptr)) }` for `text`, `lib` for `&(*lib)`), the C of the call
   without the mark. check-types.sh: 74 differ (the `mainLoop` body function), 596 only in the backend (the nodes
   of a `query(...)` loop the backend builds; calls of a non-extern generic with a written type argument,
   `processReverb`'s Float4, `widthOfNames`), 45 only in sema (method calls). Then (0.4.144): `infer_expr_type_ref` answers sema's type
   first (`c_type_from_sema`), and works one out itself only where sema's says less than it knows: a local's name
   is its declared type (sema's TypeInfo of an `opt view T` local can be `opt T`; the state of a `mainLoop` body
   is the backend's own `mod` alias), and a type that nests a reference and an optional is the backend's (sema has
   one TypeInfo for `opt view T` and for `view T` with T an optional, a reference to an optional). Measured kind
   by kind first: sema's type for any one kind left the C of 106/121/136 as it was. Over every program 8 C files
   changed: a statement or function that holds no String loses its string-pool mark (and the temporaries after
   it are renumbered), and `Array.set` of an owning element (`List`, `StringMap`, a struct of them) drops the
   value it overwrites, as the `String` instance already did; before, it leaked it. Left: an expression without a
   type as an internal error, and the expected type choosing instances (the queue lines after this step), and
   sema's TypeInfo telling an optional reference from a reference to an optional. Then (0.4.145): sema's TypeInfo of an optional reference
   (`opt view T`, a pointer that may be null) is OPT over REF, and a reference to an optional (`view T` with T an
   `opt Int`) stays REF over OPT. What had read the old spelling reads the new one: a plain reference binding
   still takes an optional reference as it is (`let x: view T = list.viewAt(...)`, #662), a `loop let x: opt view T
   in` a List(opt T) is a view of each element, an `if let` narrowing of one still unboxes, `sema_type_ref_of`
   spells it `opt view T`, the backend writes it as a pointer to T (`emit_type_ref_as_c_type`, with `const` for a
   `view` of an enum or an Any, a pointer for a fixed-width integer) and passes an optional to one by its
   payload's address. And an `if let` binding is visible in its success branch only: in the else branch the name
   is what it was outside (it was the narrowed binding there, case 1676). With that, `c_type_from_sema` keeps the
   backend's type only for a binding the backend makes itself (the state of a `mainLoop` body); a local's name
   and a type nesting a reference and an optional are sema's. No program's C changed. Then (0.4.146): a call that answers nothing has
   TYPE_VOID in sema (a resolved call, a method call, and an instance's call whose function has no result,
   `sema_type_from_instance`), so "no value" is told from "no type"; `check-types.sh` counts an expression neither
   side typed, and that sema did not even say void, as `untyped`: 2.56 million before, 111 904 after. C changes:
   207 programs lose a string-pool mark around a statement that calls a function answering nothing (none of them
   builds a String). Left untyped, for the internal error: `T.default()` (33 758), a struct, list or `none`
   literal typed only by its context (29 614, 8 358, 3 473), calls the backend binds by name (`jbField`,
   `drawRect`, `skipWhitespace`), type names written as calls (`List(...)`, `StringMapEntry`), and 3 979 names. Then
   (0.4.147): a struct, list or `none` literal with no type of its own takes the type it lands in, in
   `ensure_type_match` and as an argument of an instance's call (`list.add(value: { ... })`,
   `sema_type_from_instance`); a module-qualified call of a function with no result is void too; the type
   written as `sizeof`'s argument (`sizeof(StringMapEntry(V))`) has the type it names; and `check-types.sh` does not
   count a module or type name as untyped (`Time.now()`'s `Time`; sema reports an unknown name in a value
   position, so an untyped name that is no local and no module-level value is such a name). C: statements and
   functions that hold no String lose their string-pool mark (410 programs, 106/121/136 among them). Left
   untyped (42 237 of 111 904): `T.default()` (37 980 method calls; typing it changes about 2 000 lines of 106's C, in string-pool marks, temporaries and
   optionals, to be checked on its own), `List(...)` as a value in `decodeField`, and a few single calls; then the
   internal error.

`a defer block is not checked` is done (0.4.90). `a spawn inside a generic function` (#83491520) is step 6: fixed
before, it would be work on the template way that step 8 removes.
