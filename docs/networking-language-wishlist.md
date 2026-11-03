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

These looked like gaps during 1/12-10/12 but are not. They are listed so the
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
- **A generic "run until this holds" (5/12).** The relay test
  (`compiler/tests/cases/1269_relay_sessions`) writes each wait as its own
  `loop now < deadline and not <condition>`, because Rae has no function
  values to pass a condition in. This is already recorded and deferred
  (`docs/concurrency-model.md`, `docs/ecs-systems-and-data-observation.md`).
  The relay code itself needed none. 8/12 hit the same thing once more:
  prediction (`lib/net/replication/predictionSystem`) cannot be handed the
  app's movement function, so the app writes the replay loop (about 6
  lines, shown in the module header). Explicit is acceptable here.
- **Calling the owner while holding an alias into it (6/12).** In
  `lib/net/relay/RelayClient.rae`, `updatePaths` and `takeDirect` copy what
  they need out of a `mod DirectPath` alias (address, secret) and call
  `sendPunch(client: ...)` only after the alias's block ends. A `mod` of the
  whole client while an alias into `client.paths` is live is the aliasing
  rule working as designed. Copying out first is the intended shape.
- **A generic encoder for nested structs (7/12).**
  `lib/net/replication/ReplicaValues.rae` handles `Float`, `Int`, `Bool`,
  `Vec3` and `Quat` fields with one filtered `fields()` loop each, because
  `fields()` is one level deep. Recursing into any nested struct is already
  possible: `typeName(x)` folds to a literal in a generic body and a constant
  `if` drops the untaken arm (`docs/compile-time-reflection.md`). That is a
  `lib/` change for when a component needs it, not a language gap. (Found on
  the way, and queued as bugs, not wishes: overloads in generic bodies,
  `is` on Lists, and copyAtFallback leaks.)
