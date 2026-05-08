# Plan: Transition to Typed AST (TAST) & Robust Monomorphisation

## Objective
To resolve the "whack-a-mole" C backend issues and bridge the feature gap between the VM and Compiled targets by introducing a formal type-resolution phase and a dedicated monomorphisation (specialization) engine.

---

## Phase 1: AST Augmentation (The Foundation)
Currently, `AstExpr` and `AstDecl` contain raw tokens and names. We need to attach semantic meaning to them.

1.  **Introduce `TypeInfo` Structure:**
    - Create a canonical representation of types (Primitive, Struct, Generic Instance, Reference, Option).
    - **Type Interning:** Implement a global `TypeRegistry` to ensure every unique type (e.g., `List(Int)`) has a unique, pointer-comparable identity.
2.  **Add `ResolvedType` to `AstExpr`:**
    - Modify `AstExpr` to include a pointer to its resolved `TypeInfo`.
3.  **Symbol Linking:**
    - Add a `DeclLink` to `AstExpr` (for identifiers) that points directly to the `AstDecl` of the variable/function/type it refers to.

## Phase 2: Semantic Analysis Pass (The "Type-Checker")
Move logic out of the backends and into a dedicated `sema.c` pass.

1.  **Scope Resolution:**
    - Build a complete symbol table for every block.
    - Resolve all `AST_EXPR_IDENT` nodes to their definitions.
2.  **Type Inference:**
    - Walk the AST and assign `TypeInfo` to every expression.
    - Validate that assignments and function calls match expected types.
    - Handle `view` and `mod` reference rules at the AST level.
3.  **Implicit Conversions & Boxing:**
    - **Explicit Box/Unbox Nodes:** Insert `AST_EXPR_BOX` (to `Any`) and `AST_EXPR_UNBOX` nodes in the TAST where types are coerced to/from `Any`. This makes the C backend "dumb" regarding runtime polymorphism—it just emits the box/unbox call.
    - Explicitly insert `AST_EXPR_CAST` nodes for numeric coercions.

## Phase 3: Monomorphisation Engine (Specialization)
Instead of substituting strings during C emission, we will generate concrete AST nodes for every generic instantiation.

1.  **Specialization Registry:**
    - Maintain a table of `(GenericDecl, [GenericArgs]) -> SpecializedDecl`.
2.  **Instantiation Stack (Error Context):**
    - Maintain a stack of instantiation points during specialization.
    - If a type error occurs inside a generic (e.g., "Type 'T' has no field 'x'"), report the error trace: "Error in `List(Int)` instantiated at `main.rae:10:5`".
3.  **AST Cloning:**
    - Implement a "lazy" cloner. Only clone and specialize function bodies when they are actually called/reachable.
    - Replace generic types `T` with concrete `TypeInfo` pointers during cloning.
4.  **Mangled Naming:**
    - Generate unique, deterministic names (e.g., `rae_List_Int_add`) to prevent C symbol collisions.

## Phase 4: Backend Unification
Update backends to consume the Typed AST.

1.  **C Backend - Struct Dependency Graph:**
    - **Topological Sorting:** Before emitting C structs, build a dependency graph of all specialized structs.
    - Emit code in strict order:
        1.  Forward Declarations (`typedef struct A A;`)
        2.  Ordered Struct Definitions (Ensure embedded structs are defined before use)
        3.  Function Prototypes
        4.  Function Implementations
2.  **VM Backend:**
    - The VM compiler can now support complex features like `indexed assignment` easily because `sema` has validated the operation.
3.  **Built-ins:**
    - Treat `sizeof`, `random`, and platform intrinsics as first-class TAST nodes, ensuring consistent behavior across targets.

## Phase 5: Success Criteria (Fixing the 46 Failures)
This transition specifically targets the root causes of current failures:

| Failure Type | TAST Solution |
| :--- | :--- |
| `unknown function 'sizeof' for VM` | Resolved as a built-in Intrinsic in the Sema pass. |
| `VM currently only supports...` | Sema pass lowers `obj[idx] = v` to a standard `STORE_INDEX` op. |
| `conflicting types for externs` | Canonical `TypeInfo` ensures extern signatures are unified. |
| `C type mismatch (void* vs const*)` | TAST explicitly tracks `is_view` (const) and `is_mod` across boundaries. |
| `Incomplete type 'struct X'` | Topological sort in Phase 4 guarantees definition order. |

---

## Implementation Strategy
1.  **Step 1:** Define `TypeInfo`, `TypeRegistry`, and add to `AstExpr`.
2.  **Step 2:** Write the `resolve_types()` walk (Sema pass).
3.  **Step 3:** Implement the `specialize_generic()` cloner with Instantiation Stack.
4.  **Step 4:** Implement `topological_sort_structs()` and update C backend.
