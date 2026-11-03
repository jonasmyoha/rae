# Rae language features the networking work wanted

Each networking task (QUEUE.md, "networking N/12") ends with a short check:
was a Rae **language** feature missing that would have made the task's code
nicer, clearer, safer or faster? This file gathers the answers. It works like
`docs/ecs-language-wishlist.md`; check that file too before adding an entry,
because an ECS entry may already cover it.

Rules for an entry:

- **A language gap, not a library gap.** A helper that can be written in Rae
  today (a byte reader, a hex formatter, a ring buffer) is a `lib/` task, not
  an entry here.
- **Seen in real code.** Name the file and what you had to write instead.
- **Fits Rae's ethos (AGENTS.md).** No type inference, no `@` attributes, no
  macros or open decorator systems, no globals, nothing clever that is hard
  to analyze. If the idea goes against these rules, leave it out.
- **Nothing found means no entry.** Do not add an entry just because the
  task asks for the check.

Format: `- **Feature.** Where it was missed (file, task), what was written
instead, and what the feature would change.` Mark an entry `(landed)` when
it ships.

## Wanted

(none yet)

## Checked, not missing

These looked like gaps during 1/12-4/12 but are not. They are listed so the
next task does not propose them again.

- **Passing a struct to a C shim.** `lib/net/Udp.rae` passes a sender
  address from `recvmsg` as six `Int` words in a scratch `Buffer(Int)`. The
  `c_struct` type kind (`type b3Vec3: c_struct`, lib/box3d) already passes
  a struct by `mod` to an extern, so `NetAddress` could be one. Choosing the
  word buffer was a design choice, not a gap.
- **Encoding a plain struct to bytes field by field.** `lib/net/Wire.rae`
  writes each header field by hand. The compile-time `fields(value)` loop
  (`docs/compile-time-reflection.md`, landed) can drive a generic encoder.
  If packet headers grow, that encoder is a `lib/` task.
- **Fixed tables per connection.** `lib/net/ReliableChannel.rae` keeps
  slot fields in parallel `List(Int)`s that are made once. A `List` of a
  slot struct with `modAt` works as well. Neither needs a new feature.
- **Updating several locals from one helper (4/12).** The ChaCha20 block
  in `lib/crypto/ChaCha20Poly1305.rae` is written out round by round on 16
  locals. `mod Int` parameters on locals already work
  (`quarter(a: first, b: second)`). Writing it out was a speed choice.
- **32-bit wrapping words (4/12).** SHA-256, ChaCha20 and Poly1305 keep
  32-bit words in `Int` and mask after every add. `UInt32` arithmetic
  already wraps (`4294967295 + 2` is `1`), so the masks are a style choice.
- **Fast fixed-size limb arrays (4/12).** X25519 keeps its field elements
  in a `List(Int)` and reads them with `copyAtFallback`. It is about 18x
  slower than C, but it runs once per connection. `Array(T, cap: N)` is
  the inline fixed-size type, and `[]` is absent from both by decision
  (`docs/array-vs-list-indexing.md`). Use the vendored C library if X25519
  ever becomes the bottleneck.
