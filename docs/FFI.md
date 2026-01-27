# Rae FFI (Foreign Function Interface)

Rae provides direct interoperability with C through `extern` declarations and a native C backend.

## 1. Extern Functions

To call a C function from Rae, declare it using the `extern` keyword:

```rae
extern func abs(n: Int): ret Int
```

### Argument Mapping
- `Int` -> `int64_t`
- `Float` -> `double`
- `String` -> `const char*`
- `Bool` -> `int8_t` (0 or 1)

## 2. Struct Interop

To pass or receive C structs by value, use the `c_struct` attribute:

```rae
type Vector2: c_struct {
  x: Float
  y: Float
}

extern func DrawCircleV(center: Vector2, radius: Float, color: Color)
```

The C backend will emit these as standard C structs and pass them by value to matching C functions.

## 3. Current Limitations

- **Generic Externs:** The C backend does not yet fully support generic parameters in `extern` functions.
- **Manual Mapping:** All C functions must be manually declared in Rae.
- **Header Includes:** The C backend currently requires manual setup of include paths and linked libraries in the build command.
- **Pointer Arithmetic:** Rae does not support raw pointer arithmetic; all access must be via indexed `Buffer(T)`.

## 4. Future: FFI Generator

The goal is to provide a tool (`rae ffi-gen`) that parses C headers and automatically generates Rae `extern` definitions.

### Proposed Workflow
1. Point tool at `raylib.h`.
2. Generator identifies all `struct`, `enum`, and `function` definitions.
3. Generator produces `raylib.rae` with correct attribute mapping.
4. Support for "glue code" generation where C types don't map 1:1 to Rae primitives.
