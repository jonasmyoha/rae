# Rae Native List implementation plan

Goal: Move from C-backed List to a Rae-native `List(T)` generic struct.

## Design
`List(T)` will be defined in `lib/core.rae` as:

```rae
type List(T) {
  _items: Array(opt T)
  _length: Int
  _capacity: Int
}

func List.create(cap: Int): ret List(T) {
  ret List(T) {
    _items: Array(opt T).create(size: cap),
    _length: 0,
    _capacity: cap
  }
}

func List.add(this: mod List(T), item: T) {
  if this._length is this._capacity {
    # TODO: growth logic (needs Array resize or new Array + copy)
  }
  this._items[this._length] = item
  this._length = this._length + 1
}

func List.get(this: view List(T), index: Int): ret opt view T {
  if index < 0 or index >= this._length {
    ret none
  }
  ret view this._items[index]
}

func List.length(this: view List(T)): ret Int {
  ret this._length
}
```

## Challenges
1. **Generic Array support**: We need to ensure `Array(T)` is properly supported by the VM/C-backend.
2. **Growth logic**: Requires allocating a new larger array and copying elements over.
3. **VM implementation**: The VM currently has a native `VAL_LIST`. We need to decide if we keep it as an optimization or fully replace it with this struct.

## Tasks
- [ ] Stabilize generic struct parsing and instantiation in VM compiler.
- [ ] Add `Array(T)` as a primitive value type if not already fully supported.
- [ ] Implement `List(T)` in `lib/core.rae`.
- [ ] Update existing tests to use the new native List.
- [ ] Benchmarking vs C-backed implementation.
