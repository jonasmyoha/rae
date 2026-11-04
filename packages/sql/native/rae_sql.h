/* The C glue of the sql package (docs/sql.md): one SQLite call per function and no
 * policy. Byte arguments are a buffer, an offset and a count, as Rae passes a
 * List(UInt8); a path or SQL that SQLite needs NUL-terminated is passed with
 * the 0 already appended by Rae. Text and blobs come back by copying into a
 * buffer Rae sized from the matching *_length / column_bytes call, so this
 * glue never needs the Rae runtime's string type. Handles (sqlite3*,
 * sqlite3_stmt*) travel as integers (0: none), as lib/net's file
 * descriptors do: Rae never looks inside them, only passes them back. native/build.sh compiles this into
 * libsqlite3.a with the amalgamation and installs this header beside
 * sqlite3.h. */
#ifndef RAE_SQL_H
#define RAE_SQL_H

#include <stdint.h>

int64_t rae_sql_threadsafe(void);
int64_t rae_sql_version_number(void);

int64_t rae_sql_open(uint8_t* pathZ, int64_t flags);
int64_t rae_sql_close(int64_t db);
int64_t rae_sql_errcode(int64_t db);
int64_t rae_sql_extended_errcode(int64_t db);
int64_t rae_sql_errmsg_length(int64_t db);
void rae_sql_errmsg_copy(int64_t db, uint8_t* into);
int64_t rae_sql_errstr_length(int64_t code);
void rae_sql_errstr_copy(int64_t code, uint8_t* into);
int64_t rae_sql_busy_timeout(int64_t db, int64_t milliseconds);
int64_t rae_sql_exec(int64_t db, uint8_t* sqlZ);
int64_t rae_sql_changes(int64_t db);
int64_t rae_sql_last_insert_rowid(int64_t db);
int64_t rae_sql_autocommit(int64_t db);

int64_t rae_sql_prepare(int64_t db, uint8_t* sql, int64_t offset, int64_t count);
int64_t rae_sql_finalize(int64_t stmt);
int64_t rae_sql_reset(int64_t stmt);
int64_t rae_sql_clear_bindings(int64_t stmt);
int64_t rae_sql_step(int64_t stmt);
int64_t rae_sql_bind_parameter_count(int64_t stmt);
int64_t rae_sql_bind_parameter_index(int64_t stmt, uint8_t* nameZ);
int64_t rae_sql_bind_int(int64_t stmt, int64_t index, int64_t value);
int64_t rae_sql_bind_double(int64_t stmt, int64_t index, double value);
int64_t rae_sql_bind_text(int64_t stmt, int64_t index, uint8_t* bytes, int64_t offset, int64_t count);
int64_t rae_sql_bind_blob(int64_t stmt, int64_t index, uint8_t* bytes, int64_t offset, int64_t count);
int64_t rae_sql_bind_null(int64_t stmt, int64_t index);
int64_t rae_sql_column_count(int64_t stmt);
int64_t rae_sql_column_type(int64_t stmt, int64_t column);
int64_t rae_sql_column_int(int64_t stmt, int64_t column);
double rae_sql_column_double(int64_t stmt, int64_t column);
int64_t rae_sql_column_bytes(int64_t stmt, int64_t column);
void rae_sql_column_text_copy(int64_t stmt, int64_t column, uint8_t* into);
void rae_sql_column_blob_copy(int64_t stmt, int64_t column, uint8_t* into);
int64_t rae_sql_column_name_length(int64_t stmt, int64_t column);
void rae_sql_column_name_copy(int64_t stmt, int64_t column, uint8_t* into);

#endif
