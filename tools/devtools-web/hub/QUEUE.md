- Only announce important events using `SAY: <message>` lines. Speak nothing else.

# Rae hub — Task Queue

## Rules
- Tasks are small (30–90 min)
- Each task has Acceptance checks
- Results go to hub/RESULTS/T###.md

## Tasks

### T033 - Randomness and levels for Advanced Pong AI
- Repo: rae
- Summary:
  - Integrate `math` randomness into `advanced_pong`.
  - Randomly select an AI difficulty/level at the start of each match.
  - Make `reactionTimer` threshold and movement speed slightly random each "action".
- Acceptance:
  - AI difficulty varies between game restarts.
  - AI behavior is less mechanical and more unpredictable.

### T047 - Update `AGENTS.md` and Rae Language Specification for Naming Conventions
- Repo: `rae-lang-dev` (for `AGENTS.md`) and `rae` (for spec).
- Summary: Add hard rules for `camelCase` functions and `PascalCase` types to `AGENTS.md` and `rae/spec/rae.md`.
- Acceptance: Rules are clearly documented in both locations.

### T048 - Lexer & Parser: Generic Type Syntax `List[T]`
- Repo: `rae`
- Summary:
    - Modify `AstTypeRef` in `ast.h` to include a `generics` field (e.g., a linked list of `AstTypeRef` for type parameters).
    - Update `parse_type_ref` in `parser.c` to parse type references like `Ident '[' Type (',' Type)* ']'`.
    - Update formatter (`pretty.c`) for `List[T]`.
- Acceptance: `def x: List[Int]` parses and formats correctly, including the generic parameter.

### T049 - Parser: Generic Parameters on Type and Function Declarations
- Repo: `rae`
- Summary:
    - Modify `AstTypeDecl` and `AstFuncDecl` in `ast.h` to include a `generic_params` field (list of `Str` for generic type names like `T`, `K`, `V`).
    - Modify `parse_type_declaration` (`type List[T] { ... }`) and `parse_func_declaration` (`func identity[T](value: T): ret T`) in `parser.c` to parse these.
    - Update dumper (`ast.c`) and formatter (`pretty.c`) for these declarations.
- Acceptance: `type List[T]` and `func func[T]()` parse and format correctly.

### T050 - Parser: Collection Literal Parsing `List[T]{ ... }` and `{ ... }`
- Repo: `rae`
- Summary:
    - Implement `AST_EXPR_COLLECTION_LITERAL` in `ast.h` to represent both list and (future) map literals.
    - Modify `parser.c` to parse `List[T]{ item1, item2, ... }` explicit literal and `{ item1, item2, ... }` shorthand.
    - Implement disambiguation logic for list vs map/object literals based on presence of `:`.
    - Update dumper (`ast.c`) and formatter (`pretty.c`).
- Acceptance: `def xs: List[Int] = {1, 2, 3}` parses correctly as a list literal.

### T051 - VM Compiler: Generate Code for Collection Literals
- Repo: `rae`
- Summary:
    - Modify `vm_compiler.c` to generate VM bytecode for `AST_EXPR_COLLECTION_LITERAL`.
    - This will involve creating a `VAL_LIST` and adding elements to it.
- Acceptance: `def xs: List[Int] = {1, 2, 3}; log(xs)` executes correctly in Live mode.

### T052 - VM Compiler: Method-Call Syntax (`value.method(args...)`)
- Repo: `rae`
- Summary:
    - Refine `AST_EXPR_METHOD_CALL` handling in `vm_compiler.c`.
    - Implement logic to resolve `object.method(args...)` to the corresponding global function (e.g., `list_add`, `list_len`, etc.) and implicitly pass `object` as the first argument (`this`).
- Acceptance: `someList.add(item: 1)` compiles and runs correctly in Live mode.

### T053 - VM & C Backend: Naming Rule Enforcement (Diagnostics)
- Repo: `rae`
- Summary:
    - Implement checks in VM compiler (`vm_compiler.c`) and C backend (`c_backend.c`) to emit diagnostics for naming convention violations (camelCase for functions, PascalCase for types).
- Acceptance: Compiler warns/errors on non-camelCase functions or non-PascalCase types.

### T054 - Stdlib: `std/mem.rae` - Primitive Pointer Types and Operations
- Repo: `rae`
- Summary:
    - Define `type Ptr[T]` in `std/mem.rae`.
    - Add `extern func allocBytes`, `freeBytes`, `memmove`, `memset` to `std/mem.rae`.
    - Implement these primitives in `rae_runtime.c` (C).
- Acceptance: `std/mem.rae` compiles and can be imported.

### T055 - Stdlib: `std/list.rae` - `List[T]` Type Definition and API
- Repo: `rae`
- Summary:
    - Define the `List[T]` type in `std/list.rae` as `type List[T] { ptr: own Ptr[T], length: Int, capacity: Int }`.
    - Implement all Phase 1 API methods (`length`, `capacity`, `isEmpty`, `isNotEmpty`, `get`, `set`, `add`, `removeLast`, `clear`, `ensureCapacity`, `shrinkToFit`) in `std/list.rae` using `std/mem.rae` primitives.
    - Ensure bounds checking and value copying for `get()`.
- Acceptance: `std/list.rae` compiles and all its methods work correctly in Live mode.

### T056 - C Backend: Generate Code for `List[T]` and `Array[T]`
- Repo: `rae`
- Summary:
    - Modify `c_backend.c` to properly map `List[T]` to C representations (e.g., `struct RaeList_T { T* ptr; int64_t length; int64_t capacity; }`). This likely requires generating specialized C structs for each `List[T]` instantiation or using `void*` and careful casting where `T` is trivially copyable.
    - Generate correct C code for `std/mem.rae` externs and `std/list.rae` methods.
    - Implement deep copy for `List[T]` assignment (`=`).
- Acceptance: `List[T]` functionality works correctly in Compiled mode.

### T057 - Update Test Cases & Examples
- Repo: `rae`
- Summary:
    - Create comprehensive tests for `List[T]` functionality (parsing, API, edge cases, naming diagnostics).
    - Create `examples/list_demo.rae` with the specified content.
    - Update `random_advanced.rae` to use the new `List[T]` API with collection literals and indexing.
- Acceptance: All new and updated tests/examples pass in both Live and Compiled modes.
