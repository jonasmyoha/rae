# 15 — Taking a borrowed value

**Verdict: `handles`** — Rae prints `kept note 2, note 2`.

## The foot gun

A queue hands out an event that is still in it, and a list keeps it. If the list takes the slot's bytes,
the queue and the list own one String together and free it twice.

```rae
let note: Note = eventAt(Note, this: queue, i: 0)
kept.add(value: note)
kept.add(value: eventAt(Note, this: queue, i: 0))
```

## What Rae does

`eventAt` is a read of the queue's slot. Whatever keeps the value (a binding, an `own` parameter, a field, a
list element) gets a copy, as `=` promises everywhere; reading a field of the call copies nothing. Until
0.4.59 the binding shared the slot's bytes and the program above aborted.

## Elsewhere

See the pack: garbage-collected languages share one object, Rust refuses the move, C and a careless C++
struct free twice.
