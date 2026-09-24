# String assignment survives its source

`keep = local` must copy the String value. The loop-local source is dropped
at each iteration's end; the outer destination remains valid and owns its copy.
Ordinary assignment does not silently transfer ownership.

Before the fix, this exact three-iteration case exited 1 with empty stdout and
`POINTER_BEING_FREED_WAS_NOT_ALLOCATED` in the crash report on the test machine.
The footgun expectation was run successfully, then failed as `FIXED?` after the
backend repair. Undefined behavior can vary across allocators, so that historical
abort is not a portable promise. The fixed case deterministically prints `value2`
and exits 0 with empty stderr.

The backend copies stored String sources before releasing the previous
value, including self-assignment. Fresh produced values retain their existing
ownership-transfer path. Compiler regression 906 additionally covers borrowed
sources, struct members, repeated overwrites and interpolation.

| Elsewhere | Corresponding lifetime hazard |
|---|---|
| C | Copying a character-buffer pointer does not extend its allocation lifetime. |
| C++ | A stored `string_view` can dangle when its source string is destroyed. |
| Rust | A borrowed string cannot outlive its owner in safe code. |

Fixed by queue task #55344481; the pack records the `handles` verdict.
