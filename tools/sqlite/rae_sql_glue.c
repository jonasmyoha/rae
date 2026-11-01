/* lib/sql's glue over SQLite (rae_sql.h): each function is one SQLite call
 * (a copy is one memcpy of what SQLite returned). */
#include <string.h>
#include "sqlite3.h"
#include "rae_sql.h"

/* A handle as the pointer it holds */
#define DB(handle) ((sqlite3*)(intptr_t)(handle))
#define STMT(handle) ((sqlite3_stmt*)(intptr_t)(handle))

int64_t rae_sql_threadsafe(void) { return sqlite3_threadsafe(); }
int64_t rae_sql_version_number(void) { return sqlite3_libversion_number(); }

int64_t rae_sql_open(uint8_t* pathZ, int64_t flags) {
  sqlite3* db = NULL;
  sqlite3_open_v2((const char*)pathZ, &db, (int)flags, NULL);
  return (int64_t)(intptr_t)db;
}
int64_t rae_sql_close(int64_t db) { return sqlite3_close(DB(db)); }
int64_t rae_sql_errcode(int64_t db) { return sqlite3_errcode(DB(db)); }
int64_t rae_sql_extended_errcode(int64_t db) { return sqlite3_extended_errcode(DB(db)); }
int64_t rae_sql_errmsg_length(int64_t db) { return (int64_t)strlen(sqlite3_errmsg(DB(db))); }
void rae_sql_errmsg_copy(int64_t db, uint8_t* into) {
  const char* message = sqlite3_errmsg(DB(db));
  memcpy(into, message, strlen(message));
}
int64_t rae_sql_errstr_length(int64_t code) { return (int64_t)strlen(sqlite3_errstr((int)code)); }
void rae_sql_errstr_copy(int64_t code, uint8_t* into) {
  const char* message = sqlite3_errstr((int)code);
  memcpy(into, message, strlen(message));
}
int64_t rae_sql_busy_timeout(int64_t db, int64_t milliseconds) {
  return sqlite3_busy_timeout(DB(db), (int)milliseconds);
}
int64_t rae_sql_exec(int64_t db, uint8_t* sqlZ) {
  return sqlite3_exec(DB(db), (const char*)sqlZ, NULL, NULL, NULL);
}
int64_t rae_sql_changes(int64_t db) { return sqlite3_changes64(DB(db)); }
int64_t rae_sql_last_insert_rowid(int64_t db) { return sqlite3_last_insert_rowid(DB(db)); }
int64_t rae_sql_autocommit(int64_t db) { return sqlite3_get_autocommit(DB(db)); }

int64_t rae_sql_prepare(int64_t db, uint8_t* sql, int64_t offset, int64_t count) {
  sqlite3_stmt* stmt = NULL;
  sqlite3_prepare_v2(DB(db), (const char*)sql + offset, (int)count, &stmt, NULL);
  return (int64_t)(intptr_t)stmt;
}
int64_t rae_sql_finalize(int64_t stmt) { return sqlite3_finalize(STMT(stmt)); }
int64_t rae_sql_reset(int64_t stmt) { return sqlite3_reset(STMT(stmt)); }
int64_t rae_sql_clear_bindings(int64_t stmt) { return sqlite3_clear_bindings(STMT(stmt)); }
int64_t rae_sql_step(int64_t stmt) { return sqlite3_step(STMT(stmt)); }
int64_t rae_sql_bind_parameter_count(int64_t stmt) { return sqlite3_bind_parameter_count(STMT(stmt)); }
int64_t rae_sql_bind_parameter_index(int64_t stmt, uint8_t* nameZ) {
  return sqlite3_bind_parameter_index(STMT(stmt), (const char*)nameZ);
}
int64_t rae_sql_bind_int(int64_t stmt, int64_t index, int64_t value) {
  return sqlite3_bind_int64(STMT(stmt), (int)index, value);
}
int64_t rae_sql_bind_double(int64_t stmt, int64_t index, double value) {
  return sqlite3_bind_double(STMT(stmt), (int)index, value);
}
int64_t rae_sql_bind_text(int64_t stmt, int64_t index, uint8_t* bytes, int64_t offset, int64_t count) {
  return sqlite3_bind_text64(STMT(stmt), (int)index, count > 0 ? (const char*)bytes + offset : "",
                             (sqlite3_uint64)count, SQLITE_TRANSIENT, SQLITE_UTF8);
}
int64_t rae_sql_bind_blob(int64_t stmt, int64_t index, uint8_t* bytes, int64_t offset, int64_t count) {
  return sqlite3_bind_blob64(STMT(stmt), (int)index, count > 0 ? (const void*)(bytes + offset) : "",
                             (sqlite3_uint64)count, SQLITE_TRANSIENT);
}
int64_t rae_sql_bind_null(int64_t stmt, int64_t index) { return sqlite3_bind_null(STMT(stmt), (int)index); }
int64_t rae_sql_column_count(int64_t stmt) { return sqlite3_column_count(STMT(stmt)); }
int64_t rae_sql_column_type(int64_t stmt, int64_t column) { return sqlite3_column_type(STMT(stmt), (int)column); }
int64_t rae_sql_column_int(int64_t stmt, int64_t column) { return sqlite3_column_int64(STMT(stmt), (int)column); }
double rae_sql_column_double(int64_t stmt, int64_t column) { return sqlite3_column_double(STMT(stmt), (int)column); }
int64_t rae_sql_column_bytes(int64_t stmt, int64_t column) { return sqlite3_column_bytes(STMT(stmt), (int)column); }
void rae_sql_column_text_copy(int64_t stmt, int64_t column, uint8_t* into) {
  const unsigned char* text = sqlite3_column_text(STMT(stmt), (int)column);
  int size = sqlite3_column_bytes(STMT(stmt), (int)column);
  if (text && size > 0) memcpy(into, text, (size_t)size);
}
void rae_sql_column_blob_copy(int64_t stmt, int64_t column, uint8_t* into) {
  const void* blob = sqlite3_column_blob(STMT(stmt), (int)column);
  int size = sqlite3_column_bytes(STMT(stmt), (int)column);
  if (blob && size > 0) memcpy(into, blob, (size_t)size);
}
int64_t rae_sql_column_name_length(int64_t stmt, int64_t column) {
  const char* name = sqlite3_column_name(STMT(stmt), (int)column);
  return name ? (int64_t)strlen(name) : 0;
}
void rae_sql_column_name_copy(int64_t stmt, int64_t column, uint8_t* into) {
  const char* name = sqlite3_column_name(STMT(stmt), (int)column);
  if (name) memcpy(into, name, strlen(name));
}
