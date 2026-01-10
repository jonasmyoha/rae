This is a summary of the current state of the Rae language as per the
  existing repository, specification, and recent core rule clarifications.

  1. Current Surface Syntax and Core Rules

   * Function Syntax:
      Functions are declared with func, use named-only parameters, and require
  an explicit ret keyword for the return type and return statements.
       * func name(param: Type): ret ReturnType { ret value }
       * Note: -> is never used.
   * Variable Binding and Assignment:
       * def is used for all variable and local constant declarations.
       * = (Assignment): Always performs a copy. Observable semantics are deep
         copy; no implicit moving or sharing.
       * => (Binding): Binds or rebinds a reference-like slot (view, mod, opt)
         to a storage location. Never copies.
   * Type and Generic Syntax:
       * Types use PascalCase.
       * Generics/Type Application use parentheses: List(Int), Map(String,
         Int).
       * Brackets [] are reserved exclusively for indexing: arr[0].
   * Keywords:
      type, func, def, ret, spawn, own, view, mod, opt, if, else, match, case,
  true, false, none, and, or, not, is, pub, priv, extern, pack, default.
   * Memory / Ownership Model:
       * Types have stacked properties in canonical order: opt -> {view, mod}
         -> own -> T.
       * Value Types: T, opt T. Allowed everywhere (locals, params, returns,
         fields).
       * Borrow Types: view T (read-only), mod T (mutable). Allowed in locals
         and params.
       * Return Rules: Borrows can be returned only if derived from input
         params or this.
       * Field Restrictions: Struct fields cannot store borrows (view/mod) in
         the current version.
   * Conventions:
       * Functions are camelCase, Types are PascalCase.
       * All call arguments must be named: log(value: "hi").

  2. Status of Language Rules
   * Confirmed:
       * Assignment (=) is always a copy.
       * Binding (=>) is the only way to create an alias.
       * Parentheses for generics, Brackets for indexing.
       * Mandatory ret for functions with return values.
       * No view/mod in struct fields.
   * Not Yet Defined:
       * Standard error handling (beyond opt/none).
       * Detailed own semantics (currently a no-op type property).
       * Concurrency primitives beyond the spawn keyword.
   * Ambiguous / In Flux:
       * The transition of List(T) from a C-backed native to a pure Rae-native
         type.
       * Exact syntax for complex pattern matching (destructuring vs. simple
         matching).

  3. Minimal Valid Rae Code Examples

  Example 1: Basic Math and Return

   1 func add(a: Int, b: Int): ret Int {
   2   ret a + b
   3 }
   4 
   5 func main() {
   6   def sum: Int = add(a: 5, b: 10)
   7   log(value: sum)
   8 }

  Example 2: Copy Semantics

   1 type Point { x: Int, y: Int }
   2 
   3 func main() {
   4   def p1: Point = { x: 1, y: 1 }
   5   def p2: Point = p1 # p2 is a copy
   6   p2.x = 10
   7   log(value: p1.x) # Prints 1
   8 }

  Example 3: Binding and Borrowing

   1 func increment(val: mod Int) {
   2   val = val + 1
   3 }
   4 
   5 func main() {
   6   def x: Int = 10
   7   increment(val: mod x)
   8   log(value: x) # Prints 11
   9 }

  Example 4: Optional Borrow
   1 func check(item: opt view Int) {
   2   match item {
   3     case none { log(value: "Empty") }
   4     default { log(value: "Has value") }
   5   }
   6 }

  4. "DO NOT DO" List

   * Syntax:
       * DO NOT use -> for returns.
       * DO NOT use & or * for pointers/references (use view/mod).
       * DO NOT use List[Int] (use List(Int)).
       * DO NOT use positional arguments.
   * Assumptions:
       * DO NOT assume Rust-style move semantics; Rae copies by default.
       * DO NOT assume implicit returns.
       * DO NOT assume struct fields can hold borrows.
   * Language Comparisons:
       * Rae is NOT Rust: Borrows are explicit via => and properties, not
         sigils.
       * Rae is NOT C++: No implicit constructors or copy constructors.
