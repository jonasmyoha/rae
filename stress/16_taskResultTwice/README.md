# 16 — A task result taken twice

**Verdict: `handles`** — Rae prints `0: ["w 1"]` and stops at the second `get`.

## The foot gun

The result of a task is taken twice. A list in it would have two owners, and both free it.

```rae
if let task: view Task(List(String)) = tasks.viewAt(index: 0) {
  let got: List(String) = task.get()
}
```

run twice over the same list of tasks.

## What Rae does

`get` moves a result that owns memory out of its task, once. A second one is a run-time error that names
its line; with the task in a local it is a compile error. Until 0.4.85 both had the list and freed it.

## Elsewhere

See the pack: garbage-collected languages share one object, Rust's `join` takes the handle, C++ and C leave
it undefined.
