# Rae Standard Library Guide

This document outlines the conventions used in the Rae standard library. All new modules should adhere to these rules to ensure consistency.

## 1. Naming Conventions

### Case
- **Types:** `PascalCase` (e.g., `List`, `HashMap`).
- **Functions/Methods:** `camelCase` (e.g., `add`, `length`, `toString`).
- **Enum Members:** `PascalCase` (e.g., `Color.Red`).

### Standard Method Names
When implementing collections or common data types, use these names:
- `length()`: Return the number of elements.
- `isEmpty()`: Return `true` if length is 0.
- `contains(value)`: Return `true` if value exists.
- `clear()`: Remove all elements.
- `get(key)`: Retrieve a value (often returns `opt V`).
- `set(key, value)`: Insert or update a value.

## 2. Ownership & References

### Arguments
- Use `view T` for read-only access to a type.
- Use `mod T` for functions that modify the object.
- Pass small primitive types (`Int`, `Float`, `Bool`) by value.

### Return Values
- Functions should generally return by value.
- Returning `view` or `mod` is only allowed if the reference is derived from an input argument (ensuring the lifetime is valid).

## 3. Error Handling

- **Optional Returns:** Use `opt T` for operations that can fail gracefully (e.g., `Map.get`).
- **Result Pattern:** For complex errors, return a tuple or struct containing an error enum and the result value.
- **Panics:** Only use runtime panics for truly unrecoverable states or programmer errors (e.g., index out of bounds).

## 4. Allocation & Cleanup

- **Explicit Creation:** Types that allocate memory should have a `createType()` or `new()` function.
- **Explicit Freeing:** Until destructors are implemented, types that allocate must provide a `free()` method.
- **Buffers:** Use `Buffer(T)` primitives for raw memory management within the stdlib.

## 5. Module Structure

- Keep modules focused and small.
- Use `priv` for helper functions that should not be exposed to users.
- Provide a `main` function in examples to demonstrate usage.
