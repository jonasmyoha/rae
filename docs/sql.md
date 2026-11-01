# lib/sql — SQLite

Rae programs keep data in SQLite through `lib/sql`: `sql/Sql` (databases,
transactions, errors) and `sql/SqlStatement` (prepared statements). Every
failure is a value, an `SqlError { code, extendedCode, message }` with
SQLite's codes and the connection's message; nothing crashes. Case 1288
exercises all of it on a temporary file.

## Where SQLite comes from

- `tools/sqlite/pin` names the release (3.54.1) and the hashes of its
  amalgamation (public domain); `tools/sqlite/build.sh` downloads it into
  `~/.cache/rae/sqlite` (`$RAE_SQLITE_CACHE`), checks the SHA3-256 sqlite.org
  publishes, and builds `install/lib/libsqlite3.a` (native) and
  `install/lib/emscripten/libsqlite3.a` (browser) with Rae's glue. No SQLite
  source is in this repository; `make setup` builds it, and rebuilds it when
  the pin or the glue moved.
- The compiler links it only into a program whose C includes the glue header
  (`cheader "rae_sql.h"` in `sql/SqlGlue`), natively and in a `--target wasm`
  build; `tools/ios/gen-ios.sh` compiles the amalgamation into an iOS app that
  imports it. A program that imports `lib/sql` on a machine without the build
  stops with "run tools/sqlite/build.sh".
- Options: multi-thread mode (`SQLITE_THREADSAFE=2`; single-thread in the
  browser), API armor, no extension loading, no double-quoted strings, no
  memory statistics.

## The glue

`tools/sqlite/rae_sql_glue.c` is one SQLite call per function, no policy
(the lib/net rule). Handles are integers (0: none), never dereferenced by
Rae. Bytes go in as a `List(UInt8)`'s buffer with an offset and a count; a
path or SQL that SQLite wants NUL-terminated gets the 0 from Rae; text and
blobs come back by copying into a buffer Rae sized, so the glue does not
depend on the Rae runtime's string layout.

## The API

```rae
var database: SqlDatabase = sqlOpen(path: path, mode: SqlOpenMode.readWriteCreate, wal: true)
if sqlFailed(error: database.error) { ... }
let made: SqlError = sqlExec(database: database, sql: "CREATE TABLE ...")
var insert: SqlStatement = sqlPrepare(database: database, sql: "INSERT INTO t (a, b) VALUES (?, ?)")
let boundA: SqlError = sqlBindInt(statement: insert, index: 1, value: 7)
let boundB: SqlError = sqlBindText(statement: insert, index: 2, value: name)
let inserted: SqlError = sqlRun(statement: insert)
sqlFinalize(statement: insert)
var query: SqlStatement = sqlPrepare(database: database, sql: "SELECT a, b FROM t")
loop sqlStep(statement: query) is SqlStep.row {
  log("{sqlColumnInt(statement: query, column: 0)} {sqlColumnText(statement: query, column: 1)}")
}
sqlFinalize(statement: query)
let closed: SqlError = sqlClose(database: database)
```

- Open: `sqlOpen(path, mode, wal)` (`":memory:"` for a database in memory);
  WAL is kept by the file. A 5 s busy timeout makes a writer wait for another.
- Statements: `sqlPrepare`, `sqlBindInt` / `Float` / `Text` / `Bytes` /
  `Null` (placeholders count from 1; `sqlParameterIndex` finds `:name`),
  `sqlStep` (`row`, `done`, `failed` with `statement.error`), `sqlRun` (to
  the end), `sqlReset` (run again; bindings stay), `sqlClearBindings`,
  `sqlFinalize`. Columns count from 0: `sqlColumnInt` / `Float` / `Text` /
  `Bytes` / `Type` / `IsNull` / `Name` / `Count`; out of range reads as null.
- Transactions: `sqlBegin` (BEGIN IMMEDIATE: two writers wait at the start
  instead of failing in the middle), `sqlCommit`, `sqlRollback`,
  `sqlInTransaction`. Also `sqlChanges`, `sqlLastInsertRowId`.
- Values are bound, never spliced into SQL text.
- A statement that failed to prepare, or is finalized, and a closed
  database answer every call with an error (`SQLITE_MISUSE`, 21) and read as
  no rows.

## Threads

One connection belongs to one thread. A spawned SQL worker opens its own
connection (`sqlOpen` inside the worker) and keeps it; several connections
may use one file at once (case 1288: two workers insert 200 rows each in
their own transactions). Do not pass an `SqlDatabase` or `SqlStatement` to
another thread.
