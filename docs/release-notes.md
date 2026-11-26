# Release notes

A release is a git tag (docs/versioning-and-toolchain.md §1). Before 1.0 the
MINOR number is a named milestone; PATCH counts the work in
between. Each milestone gets a section here, newest first.

## 0.4.101, 2026-11-26: several results wherever a call can be

- The call that is taken apart can be a method call (`let wide: Int = wide, let tall: Int = tall = box.split()`), a
  call of a generic function or method (`func makeBoth(T: type, value: view T) ret value: T, fine: Bool`; each
  instance has its own results), a module's function by the module's name, and a call inside `unsafe`. A function
  with several results may wait: it runs as a resumable task like any other.
- Refused, each with its own error: `spawn` of such a function (a Task has one result), `extern` and `cExport`
  functions with several results, such a call as the subject of a `match` or an `if let` or as any other value, and
  `ret f()` that passes another function's results on (take them apart, then give each by its label).
- A local written `T` that is an Any in an instance and is moved into an `own` parameter was copied and never freed
  (found by the type matrix).
- Cases 1691, 1692, 1693; the type matrix has the source in its generic programs too (5 910 combinations).

## 0.4.100, 2026-11-26: several results that own memory

- The results of a function with several can be anything a single result can: a String, a List, a map, a struct
  that holds them, an `opt T`, an Any, an Array. `func parse(text: view String) ret value: Int, error: opt String`
  with `ret value: 0, error: "nothing to parse"` and `ret value: n, error: none` is the shape for errors.
- The caller owns what it binds, and frees it where the local's scope ends. In a `ret`, a local of the function
  moves out (also written `own name`), a place is copied (a parameter that is a view, a field), and what a call or
  a text makes is given as it is. A local named by two values of one `ret`, or still used by a `defer`, is copied.
- Case 1690 (leaks, and under AddressSanitizer); the type matrix has `one of several results` as a source (5 655
  combinations).

## 0.4.99, 2026-11-26: several labeled results

- A function can have several results, each with a label: `func divide(a: view Int, b: view Int) ret quotient: Int,
  remainder: Int`. Every `ret` gives each of them by its label (`ret quotient: a / b, remainder: a % b`); one that
  gives only some is an error.
- The call is taken apart where it is made, each local with its type and the label of the result it takes:
  `let quotient: Int = quotient, var rest: Int = remainder = divide(a: 7, b: 2)`. A local may have its label's name.
  Every result goes to a local, and such a call is no value anywhere else. `rae format` puts a local per line when
  the statement does not fit.
- For now the results are numbers, Bools, Chars, enums and structs of those, in a plain function; results that own
  memory (a String, a List), generic functions and method calls are the next steps. Each is an error that says so.
- The form was only parsed before: sema checked every value against the first result's type, and the locals of
  `let sum: result, let err: error = f()` did not exist afterwards. That untyped form is an error that shows the
  new one.
- A function with one result is compiled as before: in C, several results are one struct returned by value.
- Cases 1687, 1688, 1689; 104, 105, 202, 813 updated.

## 0.4.98, 2026-11-26: instances of generic functions are analyzed in full

- Every instance of a generic function is analyzed like a plain function: every statement, moves, frozen `let`s,
  parameter modes, main-loop rules. Before, only the first statement of the few instances sema made was. An error is
  reported once per place and names the instance: `... (in frozen(T: Int))`, with where it was made when sema made
  it. The same error at the same place is printed once, also outside generics.
- What it found in `lib/`, fixed: a changed `let` in `StringMap.remove`, `IntMap.remove`, seven queries of
  `ecs/Query`, `Tag.taggedEntities` and `QuickSort.quickSort`; parameters without a mode in `Atomic` (now `view T`)
  and `SpscRing`; `SpscRing` now opens `Atomic`. In the examples 134 and 135, a changed `let`.
- `match let` works in a generic function (it was an error of the C compiler), and `if let x: T = stored` of a stored
  optional is the error it is in a plain function (it freed what it shared twice).
- In an instance, the functions declared next to its type arguments are open (a game's hooks called from
  lib/app3d's `runWalkerGame`), a field loop over a struct without fields has no pass, and a Bool converts to and from
  a number with `as`.
- A build takes about a fifth more CPU time for this (106: 5.5 s to 6.5 s); the later steps of the design take it
  back.
- Cases 1685, 1686; docs/generic-instances-design.md.

## 0.4.97, 2026-11-26: copies of generic functions are complete

- An instance of a generic function, and each pass of a loop over fields, is a copy that shares no node with its
  template, with the type parameters written out (`compiler/src/ast_clone.c`). The elements of a list literal, a match
  statement, `own` and a loop's step were shared, so what was found for one instance stayed in the template for the
  next. The C of the 1 600 programs of this repo is the same but for one function that was emitted for a wrong
  instance's call.
- `RAE_CHECK_CLONES=1` (on in the suite) compares each copy with its template.
- Case 1684; docs/generic-instances-design.md.

## 0.4.96, 2026-11-25: the type matrix

- `compiler/tools/typeMatrix.py` (`make -C compiler type-matrix`, a stage of the suite, about 10 s): 15 types x 11
  sources x 17 positions, as plain functions and inside generic functions, checked for their values, leaks and
  AddressSanitizer. What fails today is in `compiler/tools/type-matrix-known.txt`, which only gets shorter: a spawn
  inside a generic function.
- What it found, all of it fixed:
  - `check(ok: same(value: note) is other)` and `"{same(value: note)}".length() > 0` made `same` for a Bool or an Int,
    the type wanted around it: an error of the C compiler.
  - `same(value: task.get())` was an error of the C compiler for every type but Int.
  - An Any and a String or a number in an `is`, `let x: Any = same(value: "text")`, `copyAtFallback` of a List(Any) (an
    error of the C compiler, then a leak), a `view List` with an `is` inside a function that returns a Bool.
  - `spawn f(value: own text)` leaked the String; `make() is label` leaked the result of `make` when the struct
    holds only Strings and numbers.
  - Inside generic functions: an `opt T` of their own, and a generic call in a list literal that is right ("no overload
    takes ...").
- Case 1683; docs/generic-instances-design.md.

## 0.4.95, 2026-11-25: a loop over a generic call

- A loop over what a call of a generic function or method answers (`loop let key: view String in map.keys()`,
  `in listOf(...)`, `in pair.all()`), over a field of one and over a match: they were errors ("collection loops require a
  List(T)"). `in own list` ran no pass at all, and an Array made for the loop was an error of the C compiler. What was
  made is freed when the loop is left.
- What a generic call answers is checked where it lands: `let list: List(String) = listOf(first: 3)` is an error of
  Rae's, where it was one of the C compiler.
- Generic calls in generic calls over lists (`same(list: same(list: listOf(...)))`), and `{ first: value }` with a
  `view T` that is a number, were errors of the C compiler.
- Cases 1681 and 1682.

## 0.4.94, 2026-11-25: an Any owns what it holds

- A String put in an `Any` (a let, a field, a List, a map, an Array, an optional, a result) is copied, copied again with
  the Any, and freed with it. The Any pointed at the String of somebody else, which was freed under it.
- A struct literal makes the Any of its field: a String was an error of the C compiler, and `{ value: 3 }` stored 0.
- A `view Any`, a `mod Any`, an `opt Any`, `is` of two, and a `match` that gives one work; they were errors of the C
  compiler, or read what is not there.
- The text of an Any inside a list, a map or a struct frees what it made. A UInt32 in an Any is a number (it was a
  character), and so are the other narrow integers and an enum.
- A struct, a List or a map for an `Any` is an error (`log(value)` takes any value, as before).
- Cases 1679 and 1680; docs/ownership-model.md.

## 0.4.93, 2026-11-25: text and `is` of an Array

- `log(array)`, `"{array}"` and `array.toString()` of an `Array(T, cap: N)` write its elements as a List's (`[1, 2]`),
  also inside a List, a map, a struct, an optional, a task's result and a generic function. They were errors of the C
  compiler, and `<Array>` inside another value.
- `a is b` and `equals(a:, b:)` of two Arrays, and of values that hold them, compare element by element with the rule of
  T. They were errors of the C compiler, or compared the bytes: Arrays of Strings with the same text were not equal, and
  neither were `0.0` and `-0.0`.
- An `Array(opt Task(T), cap: N)` compiles.
- Case 1678.

## 0.4.92, 2026-11-25: a search of files done again

- A file browser that searches the same words again (after a move, a trash, a change on the disk) shows what it found
  when all of it is there. A first part alone, on a slow disk or a busy machine, took the other rows away for a moment,
  and the selection with them: it went to another row, and a folder there opened.
- Case 1628.

## 0.4.91, 2026-11-25: a local that hides an outer one

- A `ret`, `break` or `continue` in a block whose local hides an outer one of the same name frees both, each once. The
  outer one leaked, and the inner one was dropped twice: a Task hung the program, a destructor ran twice.
- A move of the outer local before it is hidden is known after the block: using it there is an error (it was freed).
- A `defer` may use a name that a nested block binds again.
- Cases 1676 and 1677.

## 0.4.90, 2026-11-25: `defer`

- The statements of a `defer { }` are checked as any others: types, names, frozen `let`s, moves. They were not checked
  at all. A generic function called only in one is compiled, and a task or a taskScope in one works.
- **Breaking:** a `defer` runs where the block it is in is left, as documented: at its end, and before a `ret`, `break`
  or `continue` leaves it. It ran at the end of the function instead: also when its block was never entered, once for
  all the passes of a loop, and after the locals it used were freed. In a generic function it ran only at a `ret`.
- `ret x` hands over a copy when a defer uses `x`, and is an error when that type cannot be copied.
- A `ret` in a defer, a `break` or `continue` out of one, a defer of `main` before its `mainLoop`, and a defer that uses
  a name that is bound again after it are errors.
- Cases 1674 and 1675; docs/drop-semantics-and-defer.md.

## 0.4.89, 2026-11-25: spawns written as statements

- A `spawn work(...)` written as a statement in a taskScope is waited for at the end of the scope, and before a `ret`,
  `break` or `continue` leaves it, and its task, arguments and result are freed. Nothing waited for it before, and it
  leaked. Finished tasks are freed while the scope goes on.
- **Breaking:** outside a taskScope a spawn written as a statement is an error. Bind it
  (`let task: Task(T) = spawn ...`) or write it in a taskScope.
- A spawn in a `parallelLoop` body cannot lend a view through a taskScope outside the loop: an error of Rae's.
- Cases 1672 and 1673; docs/concurrency-model.md §3.

## 0.4.88, 2026-11-25: Arrays that own memory

- An `Array(T, cap: N)` whose elements own memory (a String, a struct, a List, a map, a generic struct, another Array) is
  copied and dropped with them wherever an Array can be: a let, a field, a List, a map, an optional, a parameter, a
  task. Most were errors of the C compiler, or of Rae's ("deep-copy is not implemented").
- `Array(List(Int), cap: 2)` makes one: the element type may have type arguments.
- A generic struct that owns memory (`Pair(List(String))`) is copied where a plain struct is: a `copy` or `own`
  parameter, a field, a List, a spawn.
- Case 1671.

## 0.4.87, 2026-11-25: optionals and generic functions

- An optional given to a generic function is its T, whole, wherever it comes from: a call's result, `own` of a local, a
  task's result. A call's was an error of the C compiler.
- The type of a generic call is known where it is given straight to another (`textOf(value: pairOf(value: 3))`,
  `textOf(value: map.values())`).
- A plain result of a generic call is wrapped where an optional is wanted (`let number: opt Int = back(value: 7)`).
- `Box(T) { held: value }` and `{ value, value }` copy an optional that is in a place; the box or list and the place
  freed the same memory.
- The text of an optional that a call hands over (`"{note()}"`, `log(task.get())`) frees what it holds, and works for a
  List, a map and a generic struct.
- `log(map.values())` frees the list.
- Cases 1668 and 1669.

## 0.4.86, 2026-11-25: a call whose result nobody keeps

- A call written as a statement (`words(count: 1)`, `list.pop()`, `map.values()`) drops its result at the end of the
  statement when that owns memory: a List, a map, a struct or an optional of those, a Task. It leaked. The same for the
  language's own calls (`enumMembers`, `fromJson`, `toBinary`, `fromBinary`, `tryGet`).
- Breaking: a function that returns a Task, called as a statement, waits for that task, as every dropped Task does.
- Breaking: `tasks.copyAt(index:)` as a statement is an error, as it is when it is kept: a Task has no copy.
- A call that returns an `opt String` can be given to an `own T` (`list.add(value: maybeText())` of a
  `List(opt String)`); it was an error of the C compiler.
- Cases 1666 and 1667.

## 0.4.85, 2026-11-25: the result of a task

- A result that owns memory (a String, a List, a struct of them) is freed with its task when nobody took it; it leaked.
- Breaking: `get` moves such a result out of its task, once. A second `get` is an error where a path reaches it, and
  stops the program with a message where the compiler cannot see it; the result was freed twice. `tryGet` after `get` is
  `none`. A number, or anything else that owns nothing, can be read again.
- `task.get()` as a statement frees what it took.
- `tryGet` of a task with an optional result, and a lent spawn of a function that returns one, work; they were errors of
  the C compiler.
- Cases 1663 and 1665, stress case 16.

## 0.4.84, 2026-11-25: spawning a generic function

- `spawn` and `spawn thread` of a generic function work, for every type and result. A String, number or optional result
  crashed in `get` or was garbage, and the call ran in place.
- `spawn value.method(...)` runs as a task like a plain call; it ran in place, and one of a generic method crashed.
- Breaking: a spawn inside a generic function is an error (its result could not be read), and so is a spawn of a method
  of the language itself (`spawn number.toString()`, a crash).
- A spawn that runs in place (a `view` of an enum) hands over a String or an optional whole.
- `List(T) { value }` in a generic function used for two struct types is the right List in each.
- `Kind.large` in a generic function used for that enum itself works again (broken in 0.4.83).
- Cases 1658 and 1659.

## 0.4.83, 2026-11-25: an enum is its own type inside another type

- A `Pair(Kind)`, `IntMap(Kind)` or `StringMap(opt Kind)` (Kind an enum) can be given to a generic function, as `view`,
  `copy` or `mod`, and comes back as what it was; `Pair(Kind)` and `Pair(Int)` can be in one program. The type checker
  had no type for an enum, only Int.
- A `List(Array(Int, cap: 2))`, a map or a generic struct of Arrays can be given to a generic function; it was an error
  at line 0. `copyAt` of an `Array(Kind, cap: N)` works.
- Functions are chosen by an enum argument: `nameOf(value: Kind)` beside `nameOf(value: Int)`.
- Breaking: a `List(Kind)` where a `List(Int)` is wanted is an error of Rae's; it was one of the C compiler. An enum is
  still read as an Int wherever an Int is wanted.
- Cases 1655, 1656 and 1657.

## 0.4.82, 2026-11-25: text and `is` of a map

- `log(map)`, `"{map}"` and `map.toString()` of a `StringMap(V)` or `IntMap(V)` write its pairs ordered by key,
  `{ "a": 1, "b": 2 }`; they were errors of the C compiler. A struct's text shows its map fields the same way; it said
  `<StringMap>`.
- `a is b` of two maps: the same keys with equal values, whatever order they were set in. It was an error of the C
  compiler.
- `is` and `equals` of two structs that hold a map compare the maps' pairs; they compared where the maps lie in memory,
  so a struct was never equal to its own copy.
- A `List`, map or generic struct of optionals (`List(opt Int)`) can be given to a `view T`; it was an error about a
  `List_void`.
- `is` of a `List(Any)`, and a struct with an `Any` field, are no longer errors of the C compiler.
- Case 1654.

## 0.4.81, 2026-11-25: maps of optionals

- `StringMap(opt V)` and `IntMap(opt V)` work for every V, tasks too. Most did not compile. A value that is `none` reads
  as a key that is not there (`get`, `viewGet`, `modGet`); `has` and `length` say the key is there.
- A struct local built by `{}` frees what it is given later; it leaked it.
- Breaking: `get` and `values` of a map that holds tasks are an error: they copied the task, and the program crashed.
  Use `viewGet` or `modGet`.
- A program's function without parameters (`values()`) no longer takes a method call of that name (`map.values()`).
- Cases 1651, 1652 and 1653.

## 0.4.80, 2026-11-25: which of several functions a call means

- A program's function named like one of the prelude (`add(list:, value:)`, `contains(list:, value:)`) is called when
  the argument names are its own, and where the names say nothing the program's wins over the lib's. The prelude's was
  called: an error of the C compiler, or without a word.
- A method call (`numbers.add(value: 3)`) is the function with `this`; a program's `add(list:, value:)` took it, and one
  that called `list.add(...)` called itself.
- Overloads of one module that differ only in their parameter names work (`pair(first:, second:)` and
  `pair(left:, right:)`), plain, generic and as methods. A plain pair did not compile, and of a generic one the first
  was always called.
- Breaking: an argument name of a generic call that no function has at that place is an error; it was read as another
  parameter's (`map.get(key: ...)` for `k`).
- Cases 1648, 1649 and 1650.

## 0.4.79, 2026-11-25: generic functions and fixed-width integers

- `mod T` of a generic function works when T is a fixed-width integer (`UInt8`, `Int32`, `UInt64` ...), and so does
  a `view T` given a `view`; they were errors of the C compiler.
- A binding that `if let x: view T` narrowed is given to a generic function as that T, not as the optional it came from.
- A `List(UInt8)` (or of another fixed-width integer) can be given to a `view T`; it was an error about a `List_void`.
- `Char` and `UInt32` in one program are two types to a generic function and a List: a Char was printed as its number,
  or the program did not compile.
- Cases 1646 and 1647.

## 0.4.78, 2026-11-25: `is` between optionals

- `a is b` and `a is not b` compare optionals: equal when both are `none`, or both are there and equal as
  `equals(a:, b:)` says; with a plain value on one side, when it is there and equal. Most were errors of the C
  compiler, and two `opt view` parameters were compared by their addresses.
- Breaking: `call() is 0`, where the call returns an `opt Int`, is false when it returned `none`; it was true.
- `contains` and `indexOf` work on a list of optionals; `sort` is an error there.
- `is` compares two generic structs (`Pair(Int)`). A `view T` or `mod T` of a generic function works when T is an
  optional.
- Breaking: a `match` on an optional is an error that names `if let` and `match let`.
- Cases 1645 and 1639.

## 0.4.77, 2026-11-25: generic parameters of an optional type

- `opt view T`, `opt mod T`, `opt own T` and `opt copy T` of a generic function, and a `List(opt T)` or another generic
  struct of `opt T`, work for every T; T is the payload. Most were errors of the C compiler.
- A call's result or a literal can be given to an `opt view` parameter of a struct or a fixed-width integer, and an
  `opt String` moved into an `opt own String`; those were errors of the C compiler.
- Breaking: a Task given to a generic `copy T` (or `copy List(T)`) is an error; it was joined and freed twice. A call
  that does not say what T is (`look(value: none)`) is an error that says how to name it.
- Cases 1642, 1643 and 1644, and 1634 (under ThreadSanitizer too).

## 0.4.76, 2026-11-25: the text of an optional reference

- `"{kind}"` and `log(kind)` of an `opt view Kind` that `if let` narrowed is the enum's name; it was its number. Of a
  List or a generic struct it was an error of the C compiler.
- One that is not narrowed (a parameter, a loop row, a call's answer) is `none` or that text, for every type; it
  crashed, was empty, or was an error of the C compiler.
- `let letter: opt Char = 'x'` compiles.
- Case 1641.

## 0.4.75, 2026-11-25: reading a list of optionals

- A `List(opt T)` can be read: `viewAt`, `modAt`, `copyAt`, `copyAtFallback`, `copyAtDefault`, `pop`, a loop over
  views, mods or copies, a struct's JSON, and generic functions. They crashed, gave a wrong value (a 7 was read as
  1) or were errors of the C compiler. A row that is `none` reads as a row that is not there (docs/collections.md).
- `contains`, `indexOf`, `sort` and `sortDesc` of such a list are errors.
- Breaking: a loop that copies tasks (`loop let task: Task(Int) in tasks`), and `copyAtDefault` or `copyAtFallback`
  of a list that holds tasks, are errors: the task was joined and freed twice. Loop over `view Task(Int)`.
- Cases 1637, 1638 (under ThreadSanitizer too) and 1639.

## 0.4.74, 2026-11-24: an optional Task

- An `opt Task(T)` works as a binding, an assignment, a return, an argument and in a list, from `spawn`, a call or
  `own`; only a struct's field did, the rest were errors of the C compiler.
- A plain value that goes into a list of optionals (`add`, `set`, `insert`, a literal) is wrapped.
- A place that holds a Task in a list literal (`{ task }`) is an error: write `own task`.
- Cases 1634 (under ThreadSanitizer too), 1635 and 1636.

## 0.4.73, 2026-11-24: `{ }` is the empty map, and a list literal only a List

- `var names: StringMap(Int) = {}` (an `IntMap`, a `ComponentTable`, any generic struct) is the empty one; it was an
  error of the C compiler.
- Breaking: a `{ }` literal of elements where no List is wanted, an empty one for a number, a String, a Bool, an enum
  or a Task, and an untyped one for an optional, are errors. What was in them was dropped (`let count: Int = { 3 }` was
  0, `let box: Box = { 3 }` a Box of its defaults, `let box: opt Box = {}` was `none`) or was an error of the C
  compiler. An optional's value says its type (`List(Int) { 1 }`), or names its fields.
- Cases 1632 and 1633.

## 0.4.72, 2026-11-24: a Task removed from a list is joined

- A `List(Task(T))` joins and frees a task it lets go of (`removeAt`, `remove`, `removeLast`, `swapRemove`, `set`,
  `truncate`, `clear`), as it did at the end of its scope: the thread was never joined and its memory leaked.
- `StringMap(Task(T))` and `IntMap(Task(T))` compile; a task they drop is joined.
- Case 1631, under ThreadSanitizer too.

## 0.4.71, 2026-11-24: the file browser picks and saves

- `mode` (`pickFile`, `pickFiles`, `pickFolder`, `save`) gives a file browser a footer with Cancel and Open, Choose or
  Save; `PickAnswered` says what came of it. `pickExtensions`: what fits, others are muted. `pickName`: the name a
  save begins with; a name that is taken asks first. In a dialog page: `"closeOnAction": false`
  (docs/file-browser-design.md section 18).
- A ListView row and a scene instance's override can set `Opacity`'s `value`.
- Cases 1629 (the thread, under ThreadSanitizer) and 1630.

## 0.4.70, 2026-11-24: the file browser's search

- `showSearch` gives a file browser a search field (Cmd+F): the folder it is at first, then what is below it, on a
  thread that is stopped when more is typed. What was found is a column of its own; "Show in Its Folder" (Cmd+R)
  opens an entry's folder (docs/file-browser-design.md section 18).
- Cases 1627 (the thread, under ThreadSanitizer) and 1628.

## 0.4.69, 2026-11-24: the file browser's sidebar

- `showSidebar` (with `sidebarWidth` and `pinned`) gives a file browser a sidebar: the user's folders, pinned and
  recent ones and the disks, as far as the browser may go to them. Where it may go anywhere a place becomes its
  root (`setBrowserRoot`), and back and forward bring back the root of then (docs/file-browser-design.md section
  18).
- A folder is kept in it by its menu; a row takes a drop; `FileBrowserState` has its `root`, `sidebarWidth`,
  `pinned` and `recent`. A program's own folders: `setKnownFolders`.
- Cases 1625 and 1626 (the latter under ThreadSanitizer).

## 0.4.68, 2026-11-24: values in literals are checked, and `when` beside a function's name

- Breaking: the value of a struct literal's field, and an element of a list literal, is checked against its type
  as a binding is: `{ layer: "page" }` for an Int is an error, and so are an Int for a Float, a Float for a
  Float64 and an optional that was not unwrapped. Only the C compiler refused some, and the numbers were
  converted silently.
- A program's function named like a value of Target (`func os(...)`) no longer breaks `when`. An error in a
  const that a `when` reads is reported in the const's file.
- 106_mobile_ui reads the time of a played track as a Float64; as a Float it was off by up to a minute.
- Cases 1622 to 1624.

## 0.4.67, 2026-11-24: `own` for a parameter that borrows

- Breaking: `own x` for a `view` or `mod` parameter is an error ("remove 'own'"), as it already was for a `copy`
  one, also in the calls of generic functions. Nothing moves there: x was counted as moved and never freed.
- Cases 1620 and 1621.

## 0.4.66, 2026-11-24: a match expression owns its value

- `let text: String = match kind { case 1 => "one {kind}" default => other }` owns its String, and a list or a
  struct the same, in every position: an arm that is a place is copied, `own x` and a fresh value are taken.
  Before, the arm's value leaked, or was shared with its place.
- The arms of a match expression are checked by the compiler; an unknown name in one reached the C compiler. An
  Int or a String needs a `default`, and an enum without one names every case. A `none` arm makes the value
  optional. The subject is evaluated once.
- In a generic function `let held: T = call()` and `let held: T = place` are right for a `T` that owns heap: a
  String was freed under the binding, and a place was shared and freed twice.
- Cases 1617 to 1619; 1617 and 1619 are in the AddressSanitizer gate.

## 0.4.65, 2026-11-24: `own x` moves wherever it stands

- Breaking: after `own x` of a local that owns heap (or a Task) a use of x is an error wherever the `own`
  stands: a binding, an assignment, a field, a list element, a `match` arm, a condition. Only an argument of an
  `own` parameter was checked; the others were accepted and freed the value twice. Two moves in one statement
  are an error as well. The same in generic functions, for the concrete type.
- A name bound in a block, a loop or an `if let` ends with it: moving it says nothing about an outer value of
  that name.
- A list handed to a generic `own T` parameter (`lists.add(value: own names)`) counts as moved.
- A value moved or dropped before `break` or `continue` is usable in the rest of the pass; it is gone after the
  loop, or at its next pass.
- `own text` of a `mod text: String` compiles.
- Cases 1612 to 1616; 1613 and 1615 are in the AddressSanitizer gate.

## 0.4.64, 2026-11-24: moves are checked in generic functions

- Breaking: a use of a value after it was moved into an `own` parameter is an error in a generic function too,
  judged by the concrete type (an Int has nothing to move). It was accepted, and the program freed the value
  twice.
- In a generic function `loop let value: T in values` and `if let value: T = values.copyAt(...)` copy the
  element when `T` owns heap; they shared it with the list, and both freed it. The `view T` forms compile for a
  String.
- Cases 1610 and 1611; 1611 is in the AddressSanitizer gate.

## 0.4.63, 2026-11-24: a destructor run by hand runs once

- Breaking: an explicit drop of a value the function does not own is an error: a field
  (`owner.handle.drop()`), a list element, a `mod` parameter or alias, a produced value. The owner drops it
  again, so its destructor ran twice. Assign a new value to release the old one early.
- `drop(this: handle)` and `Module.drop(this: handle)` consume an owned local as `handle.drop()` does, and so
  does a drop of a local or `own` parameter of type `T` in a generic function, for the concrete type. A use
  after a drop in a generic function is an error too.
- A binding of `if let`, `if var`, a collection loop or a list literal that is moved or dropped on one path
  only is freed on the others.
- Cases 1607 to 1609; 1608 is in the AddressSanitizer gate (docs/constructors-and-destructors.md section 4.3).

## 0.4.62, 2026-11-24: an explicit drop releases the fields

- `handle.drop()` on a local or an `own` parameter runs the destructor and then releases the fields, at that
  line. Before, scope exit skipped the whole value: its Strings and lists leaked, and the destructors of its
  fields never ran.
- Case 1606, also in the AddressSanitizer gate.

## 0.4.61, 2026-11-24: a parameter moved on one path is freed on the others

- An `own` or `copy` parameter that is moved, or dropped explicitly, on some paths of a function only is freed
  on the others. The move was recorded for the whole function, and the paths that did not move it leaked.
- The same for a local whose `own` String goes to an `own` parameter on one path (`ret take(text: own text)`
  inside an `if`), and for a value stored with `rae_ext_rae_buf_set` on one path.
- An `own` parameter of a generic struct (`own Box(String)`) is freed; it never was.
- The AddressSanitizer gate runs programs with threads. Case 1605, also in that gate.

## 0.4.60, 2026-11-24: an owned local moved into an optional, and lists in generic types

- `let maybe: opt Note = own note` compiles: it emitted C that the C compiler refused. An `own` String local
  moved into an `opt String` (a binding, an assignment, a field, a result) is moved; it was copied, and the
  local was never freed.
- A generic type's `List(T)` field with a generic `T` (`EventQueue(List(String))`, a `List(List(String))`
  inside) compiles: its type was registered under the name `List_String`, and the text of that type was
  emitted as Strings.
- Cases 1603 and 1604, both also in the AddressSanitizer gate.

## 0.4.59, 2026-11-24: what is taken from a slot is a copy

- A call that answers a slot someone else owns (`eventAt`, `queueAt`, a function over the raw slot read)
  gives a copy to whatever keeps its value: a binding, an `own` parameter, a field, a list element. Before,
  they shared the slot's heap, and a program that kept it, or changed a `var` of it, freed it twice and
  aborted. Reading a field of the call copies nothing.
- In unsafe code a binding of the raw slot read (`rae_ext_rae_buf_get`) handed to an `own` parameter is
  copied too; `own value` moves it out.
- `List.pop` says `ret own`: it moves its element out, so what is popped is not copied.
- Case 1602, stress 15.

## 0.4.58, 2026-11-24: drags, and files dropped from the OS

- `ui/DragSystem`: `DragSource` and `DropTarget` on any nodes, one drag in a world (`world.drag`), its events
  in `world.dragEvents`, copy or move, a label at the pointer, scrolling at the edges, canceled by Escape, a
  dialog or a menu (docs/ui-scene-format.md section 56). What the OS drags onto the window comes the same
  way, by `Gpu2d.readOsDrop`.
- In the file browser rows are dragged onto folders, columns, the trail and other browsers, a folder opens
  under a held drag, and files are dropped from the OS: call `dragSystem` (docs/file-browser-design.md
  section 18). One volume moves, across volumes and with Alt it copies. `filesDropped` tells of each.
  `fileBrowserUpdate` takes `input` as `mod`.
- A mouse press on a `DragSource` does not pan the list it is in; the wheel and the scrollbar do.
- `ui/EscapeHold`: a menu, a drag, a dialog and an edited name hold Escape together, so "Escape quits" comes
  back whatever order they end in.
- Cases 1600, 1601, check `osDrop`.

## 0.4.57, 2026-11-24: the file browser's operations

- A file browser makes folders, renames in place (the name selected without its extension), duplicates,
  copies, pastes, moves and trashes, by its keys and its context menu, with undo and redo
  (docs/file-browser-design.md section 11 and 18). A taken name asks by a dialog; a disk without a trash
  asks before it deletes; progress and failures are rows at the bottom, with a stop. What it changed shows
  at once, an open folder it renamed stays open.
- `BrowserOptions.allowOperations: false` turns all of it off. It is on by default, so **a browser of
  0.4.55 or 0.4.56 now changes the disk unless it says so.**
- `FileBrowserSystems.menuItems` are an app's own menu items. Events: `operationAsked`, `operationDone`,
  `operationRequested` (a source of the app's own), `browserMenuChosen`.
- `OperationRules.placeIn`: nothing is put outside a folder, though sources may be elsewhere.
- `fileBrowserUpdate` takes `input` (the UiInput) instead of `nowSeconds`; call `menuSystem` too.
- The Return that ends a text field or chooses a menu item is not also the list's (`keysTaken`,
  ui/KeyFocusSystem). Escape in a path field no longer clears the selection too.
- Cases 1598, 1599, check `uiFileOperations`.

## 0.4.56, 2026-11-24: file browsers in scenes, and folders without notifications

- `"FileBrowser": { "root": "~/Projects", ... }` on a scene's node opens a file browser there, with its
  options (docs/file-browser-design.md section 4.1). An app that reports unknown components before its
  first `fileBrowserUpdate` calls `applyFileBrowserComponents` before that.
- Folders that cannot be watched (more than 32 shown, or no notifications) are looked at on a thread every
  2 s while the window has the focus, and when it gets it.
- A double click on a column's divider measures the names.
- `BrowserOptions.selection` (none, single, multiple) is instead of `multiple`, and `rowHeight` is new.
  `fileBrowserUpdate` takes `windowFocused` (`Gpu2d.windowFocused()`, new), `fileBrowserRows` takes `res`.
- Cases 1596, 1597.

## 0.4.55, 2026-11-24: the column file browser

- `lib/ui/fileBrowser` (docs/file-browser-design.md): a file browser in columns that looks and changes
  nothing on disk yet. `openFileBrowser` puts one in a node, any number in a world: folders read on
  threads, live update, the keys, several selected, a bar with back, forward, the trail and a path
  field, a preview column and a large preview, columns sized by dividers, a root it stays in (also
  through symlinks), order, filter, hidden files, the tint of files opened often, entries an app
  supplies, and the state an app saves. The app reads events: `folderOpened`, `fileActivated`,
  `selectionChanged`, `entriesRequested`.
- `Text.cut: "middle"`: a text wider than its node is cut in its middle, so a name keeps its extension
  (docs/ui-scene-format.md section 55).
- A clickable node takes no click where an ancestor with `Overflow: Clip` cuts it off. Before, a row
  scrolled out of sight under another panel still took it. A node on a layer of its own is cut by none.
- A list given `ListRowNames` after its first frame could keep no rows to select. A selection moved by
  the keys says so (`ListEvent.byKey`).
- Cases 1592, 1593, 1594, 1595, check `uiFileBrowser`.

## 0.4.54, 2026-11-24: thumbnails

- `lib/Thumbnails` and `ui/ThumbnailSystem`: small pictures of files, made on threads, with a memory
  budget, a disk cache, and pictures a program makes itself (docs/ui-scene-format.md section 54).
- `Gpu2d.releaseImageKey` and `Gpu2d.registerImageBytes`: an image of the canvas can be let go of. The
  canvas finds an image by its key with a hash, not by looking through all of them.
- A JPEG with something appended (a motion photo) is read; one cut short before its picture ends is
  refused also when its EXIF thumbnail is whole.
- Spot and point lights in the deferred frame (`lib/LocalLights`, since `cfa9af3b0`).
- `lib/Gpu2dCanvas` is split (Boxes, Shutdown). Cases 1590, 1591, check `uiThumbnails`.

## 0.4.53, 2026-11-24: dividers and scrollbars

- `Divider` and `ui/DividerSystem`: a line dragged to size a pane, with limits, the resize cursor, the
  keys, a double click, and its size a field to save (docs/ui-scene-format.md section 52).
- `ScrollBar` and `ui/ScrollBarSystem`: a scrollbar for any `ScrollRoot`, both axes, shown while it
  scrolls, over its list, or always (section 53). `setScrollPosition`. Cases 1588, 1589, check `uiPanes`.

## 0.4.52, 2026-11-24: a list's rows in view only, and several scroll roots

- `ui/ListWindow`: a virtual `ListView` given only the rows in view, so 100,000 rows cost what 20 do
  (docs/ui-scene-format.md section 50). `visibleItemCount` 0: as many as the list's height holds.
- `updateScrollRoots`: every shown scroll root, a press dragging the nearest one of its axis (section
  51). The sideways scroll has its own target, `UiInput.wheelXTarget`: a program that read
  `input.wheelTarget` for `wheelX` reads `uiWheelXFor`. The vertical wheel no longer moves a sideways
  `ScrollRoot`; Shift with the wheel does. A trackpad's gesture keeps its axis.
- `ScrollMemory` keeps one root per page (two roots on one page overwrote each other's position).
  Cases 1585, 1586.

## 0.4.50, 2026-11-24: baked clouds

- The volumetric clouds can be baked into a map of the sky, baked again once a minute and shown for
  the price of the 2D layer: `lib/CloudBake`, `setCloudBake`, `CloudOptions.baked`, `bakedOnAuto` and
  `bakeSeconds`, the panel's Baked button, `RAE_CLOUD_TIER=baked` (docs/cloud-rendering-research.md
  section 8.6). Cases 1584 and 1335.
- `uploadSceneLight` takes the baked clouds as the frame shows them (`CloudBakeShown {}`: none).

## 0.4.49, 2026-11-24: key focus and list selection

- `TakesKeys` and `ui/KeyFocusSystem`: which node has the keys; a text field and a menu have them
  meanwhile (docs/ui-scene-format.md section 48).
- `ListSelection`, `SelectionStyle` and `ui/ListSelectionSystem`: selecting rows of any `ListView` by
  their key, with clicks, toggles, ranges, the keys and typing, kept through a sort, a filter and
  rows that go away (section 49). Events `selectionChanged`, `rowActivated`, `left`, `right`.
- `requestScrollToRow` has the place `nearest` and works on a flow list. Case 1583.

## 0.4.48, 2026-11-24: a caret and a selection in text fields

- A `TextField` has a caret moved by keys, words, a press and a drag, a selection that typing, paste,
  copy, cut and the deleting keys act on, undo and redo, and scrolls to its caret when its text is
  wider (docs/ui-scene-format.md section 11). Copy and cut with nothing selected still take it all.
  The pointer needs `textFieldPointerSystem` (`ui/TextFieldPointerSystem`) called before
  `textFieldSystem`. `setTextFieldSelection`, `textFieldSelection`.
- `text/Graphemes` (whole characters: marks, emoji sequences, flags, Korean) and `text/TextEdit`.
- `Sdl3.isKeyTyped`: pressed or repeated by the system; a held Backspace deletes on.
- `withoutLastCharacter` of `ui/textFieldSystem` is gone: use `previousGrapheme`. Cases 1581, 1582,
  check `uiTextField`.

## 0.4.47, 2026-11-24: `mod` is no operator, and a mode starts no statement

- `x mod y` on one line is an error that says the remainder is `%`. It was read as `x` and a
  statement `mod y` that did nothing, so the result was silently wrong.
- A statement that starts with `mod`, `view` or `own` is an error. The formatter leaves such a file
  as it is, with the error. Cases 1579 and 1580.

## 0.4.46, 2026-11-24: pointer buttons, double clicks and menus

- `Gpu2d.buttonDown/Pressed/Released/Clicks(button:)` for the primary, middle and secondary button;
  `UiPointerState` has `clicks`, `middle` and `secondary`; `UiAction.clicks`.
- An `OnClick`'s `actionIdDouble` and `actionIdTriple` are sent now (nothing sent them before), by the
  system's double-click interval; the first click's `actionId` is sent at once. `maxDelayMs` is not
  read. `OnSecondaryClick`: the right button, Control-click on macOS, a finger held still for 0.5 s.
  In `ui/InputSystem` a Control-click on macOS is no longer a primary press.
- `ui/LayerCapture` and `ui/WorldInput` follow the middle and the secondary button as pointers of
  their own (`pointerIdMiddle`, `pointerIdSecondary`).
- `ui/MenuSystem`: menus of any program (docs/ui-scene-format.md section 47). Cases 1577 and 1578,
  check `uiMenu`.
- `Gpu2d.setEscapeQuits(on: false)`: Escape does not close the window. A held Escape's repeats never
  do.

## 0.4.45, 2026-11-24: operations on files

- `lib/Files`: new folder, rename, duplicate, copy, move (also across disks), trash and delete, as an
  operation that is stepped, with a result per item, conflicts asked (skip, keep both, replace),
  cancel, undo and redo (docs/file-browser-design.md §11). Nothing is overwritten without an answer,
  a replaced thing is in the trash first, nothing has its real name before it is whole, links are
  never followed. The trash is macOS's, or the freedesktop one on Linux. Cases 1575 and 1576.

## 0.4.44, 2026-11-23: what a folder holds

- `lib/Files` is a module folder (no import changes) and says what a folder holds: `FileEntry` (kind,
  a symlink's target's kind, hidden, size, modified time), `readFolder` (names and kinds, with the
  reason a folder could not be read), `readEntryDetails`, `listFolder` with an `EntryOrder` and an
  `EntryFilter`, `pathStatus`, `mountedVolumes` (docs/file-browser-design.md §3).
- `text/NameOrder`: names ordered as people read them (numbers, case and accents aside, composed or
  not). `collections/MergeSort`: a stable O(n log n) sort by one or more keys.
- `Filesystem.moviesDir`, `musicDir`, `downloadsDir`. Cases 1571 to 1574.

## 0.4.43, 2026-11-23: an iOS app held on its side only

- `RAE_IOS_ORIENTATIONS=landscape` (tools/ios/gen-ios.sh) generates an app that opens and stays in
  landscape, either side up, on a phone and an iPad, as `RAE_ANDROID_ORIENTATION=landscape` does on
  Android. `any` (the default) and `portrait` are unchanged.

## 0.4.42, 2026-11-23: a TransformFx key a scene leaves out keeps its default

- A `TransformFx` read from a scene starts from the type's defaults, now the same as
  `defaultTransformFx()`: scale 1, rotation 0, alpha 1, visible, and `pivot` at the node's middle.
  They were 0 and false, so a scene written as Pixi writes it (an `anchor`, no `pivot`) scaled its
  node about the top-left corner: a card at scaleX 0.5 kept its left edge. A scene that wrote only
  some keys drew nothing. `pivot` is the point the scale and the turn keep (Pixi's anchor);
  `TransformFx.anchor` is not read by the transform. Case 1570.

## 0.4.41, 2026-11-23: a Sprite's hue rotation and brightness

- `Sprite.hueRotate` (degrees) and `Sprite.brightness` (1 unchanged), scene keys of the same
  names: the art's color is turned about the gray axis and scaled before the tint, as Pixi's
  ColorMatrixFilter hue and brightness do, so one blue piece of art draws red (120) or green
  (-65); a tint only multiplies and cannot. In the image shader (`gpu2d_image.wgsl` imageColor,
  the image uniform's two spare floats); `canvasSetImageColor` / `canvasClearImageColor` for
  images drawn directly, `imageColorOf` the same color in Rae. Case 1569, check spriteHueRotate.

## 0.4.40, 2026-11-23: an audio render budget holds under a busy test run

- `perfAudioBudget` (perf/PerfBudget) judges the slowest block once the slowest tenth of the
  blocks (at least one) is set aside, where it set aside the single slowest. Under the full
  suite's parallel load a few blocks are paused by the operating system, and 1456 failed its
  budget in two runs in a row while its sound held. A scene that got slower still fails: most of
  its blocks are slower (case 1480 checks both).

## 0.4.39, 2026-11-23: a turned or flipped clipping node paints its children

- A node that clips its children (`Overflow` clip, a card) and is turned by its `TransformFx`
  clips them to the window-space box round its turned box (`turnedWorldRectOf`), with rounded
  corners while the turn keeps it upright or upside down. The clip was its unturned box placed at
  its turned origin, which past a quarter turn no longer meets the card, so nothing of its
  content was drawn.
- A node flipped by a negative scale paints its children: the "scaled down to a speck" check
  took scale -1 for tiny. Every box is queued with a positive size (a negative width or height is
  the same box from its other corner, its corner radii swapped). Case 1568.

## 0.4.38, 2026-11-23: nine-slice borders shrink to fit

- A nine-slice (`canvasDrawImageKeyNineSlice`, a Sprite's `nineSlice`) in a box smaller than two
  of its borders scales all four together, s = min(1, w / (left + right), h / (top + bottom)),
  as Pixi's NineSliceSprite and CSS border-image do (`nineSliceBorders`). Each border used to be
  clamped to half the box alone, so a ribbon with 256-px caps drawn 52 high got caps of half its
  width and no middle. In a box that fits the borders nothing changes. Case 1567.

## 0.4.37, 2026-11-23: a loop over a temporary's list reads all of it

- `loop let point: view Point in outlineOf(count: 5).points` reads every element: the struct a
  call in a loop's header returns now lives for the whole loop, as a `let` would keep it. It was
  dropped at the end of the first iteration (and by a `continue`), and the loop read the first
  element and then freed memory. It is dropped once after the loop, or by a `break` or `ret`.
  Case 1566.

## 0.4.36, 2026-11-23: a hidden child no longer stretches a scroll

- `ScrollRoot`'s content extent (`scrollContentExtent`) counts only the children the layout
  places: a child switched off (`Active false`) keeps its last box, and the scroll went on as far
  as before, over empty space (a page whose sections a filter tab hides). Case 1565.

## 0.4.35, 2026-11-23: a List of List can be set

- `set`, `remove`/`removeAt`, `swapRemove` and the like on a `List(List(T))` (or a List of
  `StringMap`/`IntMap`) compile: the element they replace or take out is dropped by its
  container's own `drop`, where the C called a struct drop helper no List has and failed with
  "call to undeclared function 'rae_drop_struct_rae_List_...'". Case 1564.

## 0.4.34, 2026-11-23: UiShell runs fitSystem

- `app3d/UiShell`'s `runUiSystems` and `runScheduledUiSystems` run `fitSystem` between the layout
  and the animation step (example 106's order; ui/Pipeline's `uiSystemFit`). Without it, in every
  app on UiShell and in the 121 viewer, a `ScaleToFit` stage, `Aspect` and Overflow
  `scaleToFitY` did nothing: a 1080x2280 stage drew unscaled. Case 1563.

## 0.4.33, 2026-11-23: a layout check hides a node under an inactive parent

- `app3d/LayoutCheck`'s `layoutBoxOfEntity` (and `layoutBoxOf`) report a node hidden when any of
  its ancestors is inactive. Such a subtree is not laid out or transformed, so its nodes kept the
  boxes of an earlier pass and came back shown at a wrong place, also when read right after the
  parent's `Active` changed, before the next transform pass. Case 1562.

## 0.4.32, 2026-11-23: a scene value the decoder drops says so

- A number field of a scene written as `true` / `false`, a list or an object is a
  `wrongValueKind` scene diagnostic naming the key and the value used, where it was silently 0
  (`SafeArea`'s `"apply": {"t": true}`, the older format's spelling, applied no safe inset). A
  `null` stays the default with no diagnostic.
- `Constraints` clamps by a bound when the scene writes it: `"maxW": 430` alone sets
  `hasMaxW`. A written `has*` key still decides (`"hasMaxW": false` keeps the bound off).
  Case 1561.

## 0.4.31, 2026-11-23: tabs

- `Tab` and `TabPage` (`ui/tabSystem`, docs/ui-scene-format.md section 45): a group of tabs shows
  one page at a time. A tap on a tab selects it, the group's page of its id is active and the others
  are not, and the tab's fill takes its `selectedFill` slot; a group opens on its first tab.
  `tabSystem(world:, actions:)` each frame after input; `selectTab` and `selectedTab` for the app.
  Case 1560. Asked by the media editor's side panel.

## 0.4.30, 2026-11-23: a window goes fullscreen and back

- `Gpu2d.setWindowFullscreen(on:)` and `Gpu2d.isWindowFullscreen()`: SDL3's borderless desktop
  fullscreen, on macOS the window's own fullscreen Space (for a video editor's or player's F key).
  The flag changes at once; the new size comes with the usual `windowResized()` once the OS has
  moved the window (about 0.8 s on macOS), and leaving gives back the size and place the window
  had. A headless window only keeps the flag (case 1559).

## 0.4.29, 2026-11-20: WASM builds get a real stack

- `compiler/tools/wasm_build.sh` links an 8 MB stack (`RAE_WASM_STACK_BYTES`), for wasip1 and the
  threads target; a WASM module from `rae build` gets 1 MB; lib/wasm's `wasmDefaultLimits` gives an
  instance 1 MB of WAMR stack (was 64 KB). wasi-sdk's 64 KB stopped programs whose calls went a
  little deeper with "memory access out of bounds" (docs/wasm-modules.md, "The stack").
- `compiler/tools/wasm-stack-check.sh`: a pre-suite check that a recursion of about 400 KB of stack
  runs under Node.

## 0.4.28, 2026-11-20: `+=` and `-=`, and a loop that steps by more than one

- `x += e` and `x -= e` are statements (`x = x + e`, `x = x - e`) on a name or a field, numbers and
  Strings alike; a `let` cannot take them (docs/SYNTAX_RULES.md §4).
- A counted loop's step is `++i`, `--i`, `i += n` or `i -= n` on the loop's own number variable:
  `loop var degrees: Int = 0, degrees <= 90, degrees += 5 { }`. Anything else as the step is now an
  error (a call there used to compile and do nothing, so the loop never ended).

## 0.4.27, 2026-11-20: the walkers' keys are the world's

- The walkers' keys (WASD, arrows, Shift, jump, R, V, Page Up / Page Down) read through
  `ui/WorldInput` `worldKeyDown` / `worldKeyPressed`: they stop while a text field has focus or a
  modal shows, and no longer while the pointer only hovers the UI. `inputUpdate`, `jumpPressed`,
  `walkerResetKeyPressed` and `walkerSpawnHereKeyPressed` take the captures (breaking for a
  program that called them).
- `compiler/tools/world-input-gate.sh`: a pre-suite gate against device mouse and key reads in
  lib/app3d and packages outside its allowlist.

## 0.4.26, 2026-11-20: the world's input after the UI's

- `ui/LayerCapture`: `worldPointerStep`, `worldPointer(captures, id)` (down, pressed, released,
  place, canceled) and `worldKeys(captures)`: what the world layer gets after the layers above
  took theirs. A press on a button, or under a modal, never reaches the world, even dragged onto
  it; a modal opening over a world press ends it.
- `ui/WorldInput`: `feedWorldInput` (the device into the captures, after the layer walk),
  `worldKeyDown`, `worldKeyPressed`. The walkers feed it in `hudCapturesStep`.
- `CameraRig.updateCameraRigFor` with a `CameraRigInput` (the pointer and the keys apart;
  `updateCameraRig` stays): the walker camera's keys work while a UI button is held and stop while
  a text field has focus (app3d/HudOverlays `cameraRigInputFrom`).

## 0.4.25, 2026-11-19: reading the frame at a layer's end

- `gpu/FrameRegionReader` `frameRegionCopyNow`: copy a rectangle of the frame in the middle of
  it (the 2D flushed so far, the canvas's pass ended and started again), with the frame's own
  submission: the picture holds only what was painted before that point.
- qr 1.3.0: a `QrScanner` with `afterLayer` (`"world"`) reads at that layer's end through the
  plan (`qrScanBeforeStep`), so a code on the HUD is not read and a dialog over the middle does
  not hide the world's codes; `qrScanNow` for a program with no plan (docs/qr.md).

## 0.4.24, 2026-11-19: entities owned by peers, handed over at run time

- `net/Ownership`: an owner per shared entity that changes while the game runs. The host asks the
  owner, the owner sends its state to the new owner in one message and keeps simulating until
  the new owner takes it, and the host switches at once: every tick of an entity is one peer's,
  and its state goes on without a jump. A leaving peer's or host's entities go to the host
  (`ownershipPeerLeft`, `ownershipBecomeHost`); `ownerByNearest` is the usual rule; messages go
  as bytes over a walker session's game messages (docs/walker-net.md).

## 0.4.23, 2026-11-19: simulation levels round relevance centers

- `simulation/SimLevelSystem`: a subject (an entity id with a `SimRelevance` component) is near
  or far by its distance to a list of centers, with an enter and a larger leave distance, and
  `CameNear` / `WentFar` events the app answers (build the full entity, or keep only its record).
- `simulation/StandingSpot`: a free place to stand near a point, on the app's ground, clear of its
  colliders, within a radius, and out of a camera's view when one is free; the app answers each
  candidate. docs/simulation-levels.md (with TileStreaming's centers).

## 0.4.22, 2026-11-19: makeBox shows pictures upright

- `Mesh3d.makeBox`: a picture on any face now reads upright and the right way round: u runs to
  the right as seen from outside the face and v is 0 at its top (where the shader reads a
  picture's first row). Before, the side faces showed a picture upside down (-X and -Y mirrored,
  +X and +Y turned half round), and so did the top. A program that drew a picture made to look
  right on the old faces sees it flipped now; QR codes on cubes (example 147, the walker's QR
  cubes) now read like the code itself.

## 0.4.21, 2026-11-19: a rounded box mesh

- `mesh/RoundedBox`: a box with its vertical edges rounded (`radius`, `segments`) and a flat,
  sharp top and bottom, standing on z = 0: `makeRoundedBox` (one mesh) and
  `makeRoundedBoxParts` (the body and the four sides as meshes of their own, so a side can have
  its own material). Each side is a 0..1 UV square that shows a picture upright. Example 147 has
  a machine with its QR code on two sides.

## 0.4.20, 2026-11-19: UI layers, released

A patch release in the 0.4 line that closes the UI layers work (docs/ui-layers-design.md):
everything 2D in a window is painted and hit in one order, the window's layer stack. It sums up
0.4.15 to 0.4.19 and the layer steps since 0.4.14. `./rae toolchain pin 0.4.20` moves a project;
a program on the removed names below moves first.

- **Input through the plan and the claims.** `ui/LayerCapture`: a press is captured in layer order
  until it is let go; a modal layer or draw that opens over it cancels it (`PointerCanceled`); a
  press outside a `dismissOnOutsideTap` layer is reported (`LayerTappedOutside`); the camera, the
  touch stick, the scroll root and a game's gestures ask `uiClaimsPointer` / `uiClaimsTouch` /
  `uiClaimsKeys` first. `uiHitTest` and the walkers' input follow the same order; a shown dialog
  holds the presses under it.
- **Order inside a layer.** `LayerTag { id, z }`, `ZIndex` among siblings (`ui/ZOrder`), raised
  nodes that keep their parent's place but take no ancestor opacity, `reparentKeepingPlace`,
  `LayerScope` keeping a subtree's layers to itself, a scene's own layers, and a node's z set at a
  motion's keys (`ui/ZKeys`).
- **Effects and page transitions.** Safe rectangles, draw areas as clips, world anchors sorted far
  to near, the backdrop blur per layer, isolated layers composited once at their opacity, and page
  transitions painting both worlds by one plan.
- **The moved programs.** The "..." menu on the dialog layer, the editor panel and the walkers'
  overlays by a plan, 106 and 121 on the stack, the pong and tetris boards as program draws under
  the scene UI.
- **LayerTag is the one link from a node to its layer** (0.4.17, breaking). Removed: `LayerRoot`,
  `LayerRef`, `collectPaintableRoots`, `createStandardLayers`, `createLayerEntity`,
  `findLayerEntity`, `entityLayerOrder`, `raiseTaggedNodes`, `layerDialogOrder`,
  `gpu2dLayerRange` and `ui/LayerLegacy`; "content" and "back-button" are now "hud" and "dock".
- **Seeing the layers.** The debug overlay's Layers page (`ui/DebugLayers`), `layerReportRows`, and
  121's Layers panel with inspector edits of LayerTag, ZIndex and CustomPaint (0.4.18).
- **Measured** on an iPhone 15 Pro against the commit before the layers: the same frame times, CPU
  time and draw calls (0.4.19, `RAE_GPU2D_DRAWS=1`).
- Packages: qr 1.2.0 (a QrCode node painted in its place in the order, needs Rae 0.4.13).

## 0.4.19, 2026-11-19: the UI layers measured on a phone

- `RAE_GPU2D_DRAWS=1`: the 2D canvas logs its draw calls a frame (docs/profiling.md).
- The UI layers measured on an iPhone 15 Pro against the commit before them: the same frame times,
  CPU time and draw calls for 106 and a walker (docs/ui-layers-design.md §11).

## 0.4.18, 2026-11-18: the layers in the debug overlay and the UI editor

- The debug overlay's dialog has a Layers row: the window's layers in paint order with what is
  in each (`ui/DebugLayers` `feedDebugLayers`, `RAE_DEBUG_LAYERS=1`). The walkers feed it.
- `ui/LayerReport` `layerReportRows`: the report as rows, each with its layer, node or draw.
- `componentNamesFor` lists `LayerTag`.
- Example 121: a Layers panel (L) for the edited document, and inspector edits of `LayerTag`,
  `ZIndex` and `CustomPaint`.

## 0.4.17, 2026-11-18: LayerTag is the one link from a node to its layer

A patch release in the 0.4 line, breaking. `./rae toolchain pin 0.4.17` moves a project.

- A node names its layer only by `LayerTag { id, z }` (a .raescene's `"LayerTag": { "id":
  "dialog" }`, or `setLayerById` / `raiseNodeToLayer` in code, which set the tag and keep its z).
  The name is looked up in the window's `LayerStack`; a root without a tag is in hud, and a name
  the stack has not is painted in hud. A node inside a scene whose tag names a layer is raised
  (`isRaisedNode`), unless it is below a `LayerScope`.
- Removed (breaking): the layer entities `LayerRoot` and `LayerRef` (`UiWorld.layerRoots`,
  `layerRefs`), `createStandardLayers`, `createLayerEntity`, `findLayerEntity`,
  `entityLayerOrder`, `collectPaintableRoots`, `raiseTaggedNodes`, `layerDialogOrder`,
  `gpu2dLayerRange`, and `ui/LayerLegacy` (`adoptWorldLayers`, `stackOrderOfLegacy`).
  `raiseNodeToLayer` takes no `order`. A program that made its own layers either tags its roots
  with the standard names (121: the document in hud, its guides in hudOver, the chrome in dock) or
  adds its layers to the stack it keeps (`addLayer`). The old names "content" and "back-button"
  are no longer read as hud and dock: write "hud" and "dock".
- `uiHitTest` walks the roots in the order of the standard stack's plan (`rootsInPaintOrder`).
  A layer contribution notices a LayerTag that changed its layer or z on its next refresh.

## 0.4.16, 2026-11-18: a screen tint

- `setScreenTint(cache: renderer.passes.transparent, r, g, b, strength)`: a color over the whole
  lit frame after the transparents (one full-screen triangle; nothing drawn at strength 0).

## 0.4.15, 2026-11-18: the transparent pass takes several meshes a frame

- `lib/TransparentForward` draws each run of one mesh in the order queued (`transparentRuns`);
  style 4 is a shimmering surface (`addTransparentShimmerInstance`, `TransparentShimmer`); the blend
  is premultiplied, so a style can be additive. `Mesh3d.makeOpenCylinder`. Styles 2 and 3 (the
  explosions' puff and glow) now run as designed.

## 0.4.14, 2026-11-18: the 3D shell's overlays by the layer plan; HudOverlay removed

A patch release in the 0.4 line. `./rae toolchain pin 0.4.14` moves a project.

- `app3d/HudOverlays`: the shell draws everything 2D by the window's layer stack:
  `HudLayerPaint` (`createHudLayerPaint`, `drawHudLayers`, `buildHudLayerPlan`) puts the world's
  labels in the world layer (game mode), the scene UI's roots in their layers, the log strip in
  the log layer (editor mode) and the debug readout in the debug layer. An app's own drawing is a
  `LayerDraw` it adds. The walkers keep it on their `WalkerRender`.
- Removed (breaking): `ui/HudOverlay` (`HudOverlay`, `OverlayDraw`, `OverlayShows`,
  `overlaysInOrder`, `UiWorld.hudOverlays`) and `addStandardHudOverlays`, `drawRaeOverlay`,
  `drawOverlays`. A program on them moves to a `LayerDraw` in its layer.
- `setLayerShows` paints a layer in one mode only.

## 0.4.13, 2026-11-18: UI layers, the first steps

A patch release in the 0.4 line. `./rae toolchain pin 0.4.13` moves a project. The first steps of
docs/ui-layers-design.md (one paint and input order for everything 2D); the rest is queued.

- `ui/UiLayers`: one LayerStack per window (the standard layers world, hud, hudOver, dock, popover,
  toast, dialog, cover, notification, log, debug; a program's own in a gap), several UI worlds
  joining it, each world's contribution, and the paint plan (`buildLayerPaintPlan`). `LayerTag`
  gains `z`; a scene's LayerTag now raises its node by itself (`raiseTaggedNodes`).
- `renderSystemGpu2d` paints by the plan; `Gpu2dCanvas` has a layer push (`canvasPushLayer`:
  opacity and offset). `collectPaintableRoots` sorts stably.
- `ui/LayerHit`: a hit walk down the plan over every world (surfaces, modal layers and draws, the
  layers a press missed, drop targets).
- `ZIndex` orders siblings in paint and hit (`ui/ZOrder`).
- A cover layer hides what is under it; `ui/LayerMotion` eases a layer's opacity and offsets.
- `CustomPaint`: a node the program draws in its own place in the order (plan steps,
  `beginCustomPaintG` / `endCustomPaintG`); `ui/LayerReport` lists what is drawn in which layer.
- qr 1.2.0: a QrCode node is a CustomPaint of kind `qrCustomPaintKind`, painted by
  `qrCodePaintStep` in its place (under a dialog over it); `qrCodePaint` still works.

## 0.4.12, 2026-11-18: tile streaming with a primary center

A patch release in the 0.4 line. `./rae toolchain pin 0.4.12` moves a project.

- app3d/TileStreaming `streamTilesPrimary` and `TileGridSettings.priority` (TilePriority): the
  first center (a fast camera) gets the slots first, round it and where it is heading; the other
  centers keep a smaller reach; a full set of slots gives up its least needed tile to a missing
  one needed clearly more. Off by default (docs/open-world-tiles.md).
- `streamPropTiles` takes `deltaTime` (breaking: one more argument); the walkers' forest streams
  with the camera's focus primary.

## 0.4.11, 2026-11-18: middle-button pan and wheel zoom on a canvas

A patch release in the 0.4 line. `./rae toolchain pin 0.4.11` moves a project.

- A drag with the middle mouse button pans a canvas (ui/canvasGestureSystem), begun
  anywhere inside it, over a node too, and coasts when let go
  (`CanvasGestures.middleDown`).
- `CanvasControls.wheelZooms`: the mouse wheel zooms about the pointer with no modifier held
  (a sideways wheel still pans). Off by default (docs/ui-scene-format.md §41). Example 152
  uses both.

## 0.4.10, 2026-11-17: reliable messages no longer lost past the receive window

A patch release in the 0.4 line. `./rae toolchain pin 0.4.10` moves a project.

- lib/net's reliable channel could lose messages while the sender saw them delivered:
  the sender's window starts at its oldest unacked fragment, the receiver's at the first
  fragment of the oldest message it has not delivered, so a fragment beyond the receiver's
  window (after a message of several fragments, or while the app had not taken its
  messages) was refused and its packet still acked. Such a packet is now left unacked and
  the fragment comes again (`ReliableChannel.reliableIsAhead`, `Connection`). A run of one
  6-fragment message then 100 small ones through loss, reordering and duplicates stopped at
  59 on half the seeds; it now delivers everything on all of them.

## 0.4.9, 2026-11-17: smooth canvas zoom

A patch release in the 0.4 line. `./rae toolchain pin 0.4.9` moves a project.

- `CanvasControls.zoomEaseSeconds`: a wheel notch or a zoom key eases the camera to its
  target zoom, the point under the pointer staying put on every frame; `keyZoom` is the step
  of the new `=` / `-` zoom keys (`CanvasGestures.zoomKeys`); `pinchCoasts` lets a released
  pinch keep its zoom speed and slow with `friction`. All off by default
  (docs/ui-scene-format.md §41). Example 152 uses them.

## 0.4.8, 2026-11-17: zoom behavior per node

A patch release in the 0.4 line. `./rae toolchain pin 0.4.8` moves a project.

- `ZoomBehavior` (lib/ui/ZoomComponents, docs/ui-scene-format.md §44): under a Camera2D a
  node `Scales` with the zoom (the default), stays `Fixed` on screen, or is `Clamped`
  between two sizes, about a pivot; `showFrom` / `showTo` show it in a range of zooms with
  a cross-fade centered on each bound, so levels of detail cross-fade (or switch, with
  `fade: 1`). Applied by the transform pass.
- Example 152_zoomCanvas: a node canvas in the analytical and the zoomy feel.
- A UiWorld has one more table (`zoomBehaviors`).

## 0.4.7, 2026-11-17: line breaks in a Text

A patch release in the 0.4 line. `./rae toolchain pin 0.4.7` moves a project.

- A `"\n"` in `Text.text` is a hard line break (`"\r\n"` and `"\r"` count as one):
  each line one line height (the style's `lineHeight`) below the one before, aligned on
  its own; a `Hug` text measures the widest line and every line's height
  (docs/ui-scene-format.md §43). It used to draw every part on one line with nothing
  between them. A wrapping text treats `"\r\n"` as one break too.
- `SdfText`: `sdfHasHardBreak`, `sdfHardLines`, `sdfMeasureLines`.

## 0.4.6, 2026-11-17: text outlines in the theme

A patch release in the 0.4 line. `./rae toolchain pin 0.4.6` moves a project.

- A theme has `outline` tokens (`color`: a palette name or rgba, `width` in points,
  `extends`), and a text style names one with `"outline": "<token>"`, inherited through
  the style's `extends` (docs/ui-scene-format.md §42). The renderer draws the shadow,
  the outline, then the glyphs, from the font's distance field.
- A text style naming an `outline` or `shadow` token the theme does not declare is an
  `unknownToken` scene diagnostic (`ui/ThemeParse reportThemeTokenDiagnostics`, run by
  `app3d/UiShell` and the UI editor); before, a misspelled shadow was silently dropped.
- `lib/ui/ThemeParse` is a module folder now; imports are unchanged.
- Also since 0.4.5: an instance override may set `Layout.columns` / `gap`; a call that
  returns a reference cannot be the argument of a by-value parameter (bind it first).

## 0.4.5, 2026-11-17: saving edits into a frame

A patch release in the 0.4 line. `./rae toolchain pin 0.4.5` moves a project.

- `ui/FrameWriter` (new): `writeFrameEdits` saves edits made while a frame is shown
  into that frame's override bags (docs/ui-coordinate-and-responsive-layout.md §2): each
  edited component whole, out of the bag when it equals the base, a removed one in the
  bag's `remove` list. The rest of the file stays byte for byte.
- `ui/SceneWriter`: `writeSceneTextWith(..., edits: SceneEdits)` writes from the world
  only the components an editor names (`"<nodeId>/<Component>"`); every other one stays
  the author's text, so a camera's or an effect's runtime values are not saved.
  `writeSceneText` is unchanged in what it writes.
- Changed: a save keeps a header the author wrote after `nodes` (a `frames` or `states`
  block) after `nodes`; it used to move every header before them.
- The UI editor (121) saves an edit made in a frame into that frame.

## 0.4.4, 2026-11-17: frames picked by the whole window

A patch release in the 0.4 line. `./rae toolchain pin 0.4.4` moves a project.

- A scene frame's `when` (`ui/Frames`, docs/ui-coordinate-and-responsive-layout.md §3)
  names `minWidthPt` / `maxWidthPt`, `minHeightPt` / `maxHeightPt`, `orientation`
  (`portrait` / `landscape`), `pointer` and `inputType` (`touch` / `pointer`).
  `minPtW` still works as the older spelling of `minWidthPt`.
- The most specific matching frame wins (the most conditions, then the largest
  minimum width, then the smallest maximum width), then the one declared first; then
  the `default` frame. `forcedFrameId` overrides all of it.
- A frame may declare its reference `viewport: { "w": 393, "h": 852 }` in points
  (`FrameSet.viewportWs` / `viewportHs`).
- `FrameWindow { widthPt, heightPt, pointer }`, `selectFrameForWindow` and
  `applySceneFramesForWindow` pick and apply by the window's width and height. The
  width-only functions stay; with them a height or orientation condition does not hold.
- `lib/ui/Frames` is a module folder now; imports are unchanged.

## 0.4.3, 2026-11-17: one UI coordinate model, in points

A patch release in the 0.4 line. `./rae toolchain pin 0.4.3` moves a project
(and `./rae update physicsScenes` to 1.2.1). docs/ui-coordinate-and-responsive-layout.md
is the standard; its first rule changed on 2026-11-17 from 3 units per point to:

- **A design unit is a logical point**, the unit of iOS, Android (dp), CSS and Figma
  (`ui/DesignUnits`, `unitScale` 1). A phone is about 393 wide, a margin 16, body text
  17, a touch target 44. Every UI size, scene and theme is in points.
- **The layout fills the window** in points: a bigger window shows more, never a bigger
  UI. `app3d/UiShell` sets it (`syncDesignResolution`, `fitUiToWindow`,
  `resizeUiToWindow`); a program does not set its own design resolution.
- **A user zoom**, written `1.25x` (`UiWorld.uiZoom`, `ui/UiZoom`; Ctrl/Cmd + - 0 with
  `UiShell.handleUiZoomKeys`): the layout is the window / zoom, the pointer, safe area
  and size class follow.
- **A fixed art-directed area** (a game table, a playfield) is a stage: a `ScaleToFit`
  subtree in a scene (modes `Contain`, `Cover`, new `FitHeight`), or on the immediate
  canvas `Gpu2dCanvas` `stageFit` + `canvasPushStage` / `canvasPopStage` (new).
  Inside a scene's stage, `StageEdge` (new) pins a node to the visible or safe window
  edge or bleeds it over the whole window, and a `SafeArea` keeps the notch's size.
- `Overflow` / `OverflowPolicy` `ScaleToFitY` (with `minScale`) shrinks a column whose
  content is too tall, as Pixi's.
- `ui/ResponsiveExtent` (new): the window-filling layout of an app with a phone page
  column, by size class, with a side panel when expanded and a device preset emulated.

**What 0.4.3 changes for a program that ran on 0.4.2**

- lib/ui's fallback sizes are points: a text style its theme does not size is 17 (was
  43.37), `SheetPanel.rowHeight` 56 (was 121.44), `UiWorld`'s default layout 393x852.
- `DebugOverlay` has no `scale` and `setDebugOverlayScale` is gone; the perf HUD's draw
  functions lost their `scale` parameter. The user zoom enlarges them.
- `app3d/StaminaWheel.drawStaminaWheel` takes `camera:` (0.4.1).
- A scene that declares a unit (a top-level `"unitScale"`) is refused by the repository's
  gate; `tools/scale-ui-lengths.py --divide 3` converts a scene or theme authored at 3
  units per point (it reports any number it does not know).
- `lib/ui/Components` and `Gpu2dCanvas` are module folders now; imports are unchanged.

## 0.4.2, 2026-11-17: kinematic bodies stop at their pose

A patch release in the 0.4 line. `./rae toolchain pin 0.4.2` moves a project.

- A kinematic box3d/ecs body whose Transform3D is written once a frame now
  stops at that pose when the frame runs several fixed steps. The push gave it
  the velocity that reaches the pose in one step and left it there for the
  frame's other steps, so it overshot, and at 3 or more steps a frame swung to
  the world's top speed. `RigidBody` remembers the pose; on a step with no new
  pose the body is aimed at it again and stopped once there. A program that
  touched every kinematic Transform3D before each step to avoid this can stop.
- A kinematic body riding a platform can end elsewhere than before in a
  program that ran several steps a frame (the platform no longer overshoots).

## 0.4.1, 2026-11-17: a camera that rolls

A patch release in the 0.4 line. `./rae toolchain pin 0.4.1` moves a project.

- `Camera3d` has a `roll`: radians about the view direction, the right side
  down above 0 (a drone banking into a right turn). It defaults to `0.0`, and
  at 0 every view is exactly what it was.
- `Scene3d.cameraViewMatrix(camera:)` is the camera's view matrix with its
  roll; the deferred and forward frames and the underwater fog build their view
  with it. `Math3d.viewUp(eye:, target:, roll:)` is the up reference it hands
  `mat4LookAt`.
- `app3d/WorldLabel.projectCameraPoint(camera:, aspect:, point:)` projects a
  point the way the frame is drawn, roll included: use it for labels and a
  reticle. `projectToScreen(eye:, target:, ...)` stays the unrolled form.
- Changed: `app3d/StaminaWheel.drawStaminaWheel` takes `camera:` in place of
  `eye:`, `target:` and `fovYDeg:`.
- Unchanged with a roll, on purpose: the shadow cascades (spheres round the
  view axis) and the camera-facing leaf cards and water drops, which stay
  upright in the world.

## 0.4.0, 2026-11-17: module folders

Named on 2026-11-16 and built in the 0.3 line from 0.3.173; the 0.3 line ended
at 0.3.178 (tag `v0.3.178`). docs/module-model-options.md is the design. A
project moves with `./rae toolchain pin 0.4.0`, then takes its packages' new
versions with `./rae update <name>` (physicsScenes 1.2.0, qr 1.1.0).

**What 0.4.0 refuses that 0.3.x took**
- An import or qualifier whose upper and lower case differ from the file or
  folder on disk (`import vec3`, `import sys`, `math.sqrt`, `sdl3.isKeyPressed`,
  `Particles.tick()` for the folder `particles/`): write it as on disk
  (`import Vec3`, `Math.sqrt`, `particles.tick()`). These worked on macOS only.
- A file and a folder of one name in one place (`Hud.rae` beside `Hud/`), at
  any depth: keep one, or move the file into the folder.
- An import of a file inside a module folder (`import Gpu2d/Window`): import
  the module (`import Gpu2d`).
- The same function with the same parameters in two files of one module.
- `Gpu2dImages.` and `import Gpu2dImages`, which existed in 0.3.172 only: write
  `Gpu2d.` and drop the import.
- The old imports of the ui system modules that became folders; each folder is
  one module now, and a call qualified by one of its files' names takes the
  module's name (`RenderFills.paintGradient` -> `RenderSystem.paintGradient`):

  | 0.3.x import (every file of the folder) | 0.4.0 import |
  |---|---|
  | `ui/renderSystem/RenderSystem`, `RenderFills`, `RenderSprite`, `RenderCull`, `RenderText` and the 13 others | `ui/RenderSystem` |
  | `ui/animationSystem/...` (8 files) | `ui/AnimationSystem` |
  | `ui/buttonSystem/...` (4 files) | `ui/ButtonSystem` |
  | `ui/pageTransitionSystem/...` (3 files) | `ui/PageTransitionSystem` |
  | `ui/inputSystem/...` | `ui/InputSystem` |
  | `ui/layoutSystem/...` | `ui/LayoutSystem` |
  | `ui/listMotionSystem/...` | `ui/ListMotionSystem` |
  | `ui/listReorderSystem/...` | `ui/ListReorderSystem` |
  | `ui/listViewSystem/...` | `ui/ListViewSystem` |
  | `ui/scrollRootSystem/...` | `ui/ScrollRootSystem` |

  The one-file system folders (`ui/dialogSystem/DialogSystem`, the lib/ecs
  systems and the rest) keep their import paths.

**Modules**
- A module is a PascalCase file or a PascalCase folder (0.3.174): `import
  Gpu2d` finds lib/Gpu2d.rae or the folder lib/Gpu2d/, whose files are one
  module. A module that reaches the 1,000-line cap becomes a folder with no
  change to the code that uses it. Refused: a file and a folder of one name, an
  import of a file inside a module folder, the same function in two files of
  one module. A program's PascalCase folders are module folders too (0.3.178),
  seen with no import like every program file.
- Imports and qualifiers are spelled as on disk, with the same upper and lower
  case, on every system (0.3.173): `import vec3` and `math.sqrt` are refused
  with the right spelling. They used to work on macOS only.
- lib/Gpu2d is a module folder (0.3.175), and its image functions are back in
  it: `Gpu2d.loadImageKey`, `Gpu2d.drawImageKey` and the rest.
  **`Gpu2dImages.` existed only in 0.3.172**; code written against it goes back
  to `Gpu2d.` and drops `import Gpu2dImages`.
- The ui system folders that held several modules are one module each
  (0.3.177): `import ui/RenderSystem` replaces `import ui/renderSystem/RenderSystem`
  and its 17 sibling imports, and `RenderFills.`, `RenderSprite.`, `RenderCull.`
  calls are `RenderSystem.`; likewise ui/AnimationSystem, ButtonSystem,
  PageTransitionSystem, InputSystem, LayoutSystem, ListMotionSystem,
  ListReorderSystem, ListViewSystem and ScrollRootSystem. physicsScenes 1.2.0
  and qr 1.1.0 use them.
- The rules follow (0.3.176): AGENTS.md says a module is a PascalCase file or
  folder, a system is a module of its own, and a module at the line cap becomes
  a folder; "package" means a `.raepack` package only. `rae init` writes these
  module rules into a new project's AGENTS.md. `run_cases.sh --grep` given a
  file of a module folder searches for its module; `--list` prints the cases.

## 0.2.0, 2026-10-11: physics and servers

The first milestone after 0.1.0 (2026-09-12). The 0.1 line ended at 0.1.268
(tag `v0.1.268`). A project whose `.raepack` requires `rae: { version: "0.1" }`
(or any `0.1.x`) must change it to `"0.2"`.

**Physics**
- Box3D's C library is Rae's physics engine, through generated bindings
  (`lib/box3d`) and an ECS layer (`lib/box3d/ecs`), with examples for the
  playground and a driven vehicle. The Rae port of Box3D (`lib/physics`) stays
  as a frozen reference and the portable fallback
  (docs/physics-two-implementations.md).
- A physics scaling benchmark for both tracks (`benchmarks/physics_scaling`).

**Networking and servers**
- `lib/net`: TCP sockets, a readiness poller (kqueue on Apple and BSD, epoll
  on Linux) and byte buffers, all Rae over one-call system shims.
- `lib/http`: an HTTP/1.1 parser and response encoder in plain Rae.
- `lib/webSocket`: the upgrade, frame decoder and encoder in plain Rae.
- `lib/net/ecs`: HTTP and WebSocket servers as ECS systems.
- Server benchmarks against tuned Rust, Node and Bun (`benchmarks/servers`,
  and three tabs in the devtools Benchmarks view): Rae is in the same range,
  ahead on single-core HTTP throughput.

**Tasks and concurrency**
- Spawned tasks run M:N on the worker pool, scheduled in Rae; `get()`,
  `sleep` and socket waits suspend a task instead of holding a thread.
- `blocking` externs run a long C call on its own thread.
- A spawn inside a `taskScope` lends a view instead of copying it.
- In the browser, waiting tasks run between `mainLoop` frames.

**Language**
- `const` / `let` / `var`, with a `let` frozen as a whole; `if var`.
- Compile-time `when` and the `Target` prelude for per-platform code;
  `rae build --check-targets`.
- `match let`, `equals`, `enumName` / `enumMembers` / `enumCount`, `toString`
  for lists, optionals and structs, one-UTF-8-character `Char` literals.
- SIMD: `Float4` masked tails and `Float8` (AVX2 on x86-64).
- Many fixes in generics, ownership and drops, and sema checks that reject
  wrong members and qualifiers.

**Toolchain**
- The Live / VM target is gone: the C backend is the one target.
- `rae format` and the build's format preflight own the layout.
- Projects declare the Rae they need; `rae toolchain`; dependencies with
  `rae add` / `fetch` / `update` / `tree`.
- The compiler builds with -O2 and LTO, and emitting is linear.
- Browser builds of WebGPU apps, with WASM SIMD and a frame-callback loop.
- The generated C compiles without warnings; gates for TSan, ASan, banned
  terms and stress cases run before every suite.
- Runtime logic moved from C to Rae (strings, JSON, time, logging); C is kept
  for platform calls, and a faster platform implementation wins where one
  exists (CommonCrypto SHA-1).
