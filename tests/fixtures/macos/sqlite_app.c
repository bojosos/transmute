/* Project-authored fixture: uses the guest SQLite runtime (/usr/lib/libsqlite3.dylib built by
 * scripts/build_alchemy_sqlite.ps1) the way Krita's resource database, darktable's library and
 * Python's sqlite3 module do: a file database in WAL mode (POSIX record locks and the shared-memory
 * mapping of the unix VFS) with a second connection, prepared statements and bindings, FTS5, JSON
 * and math functions, an application-defined function and collation (guest callbacks called from
 * SQLite), the blob and backup APIs, and serialize/deserialize. The SQLite declarations below are
 * written from SQLite's documented C interface. */
typedef long long sqlite3_int64;
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef struct sqlite3_context sqlite3_context;
typedef struct sqlite3_value sqlite3_value;
typedef struct sqlite3_blob sqlite3_blob;
typedef struct sqlite3_backup sqlite3_backup;
enum { SQLITE_OK = 0, SQLITE_ROW = 100, SQLITE_DONE = 101, SQLITE_UTF8 = 1,
       SQLITE_OPEN_READWRITE = 0x2, SQLITE_OPEN_CREATE = 0x4, SQLITE_OPEN_FULLMUTEX = 0x10000,
       SQLITE_DETERMINISTIC = 0x800 };
#define SQLITE_TRANSIENT ((void (*)(void*))-1)
extern const char* sqlite3_libversion(void);
extern int sqlite3_threadsafe(void);
extern int sqlite3_open_v2(const char* name, sqlite3** db, int flags, const char* vfs);
extern int sqlite3_open(const char* name, sqlite3** db);
extern int sqlite3_close(sqlite3* db);
extern int sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* user, char** error);
extern int sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** statement, const char** tail);
extern int sqlite3_bind_int64(sqlite3_stmt* statement, int index, sqlite3_int64 value);
extern int sqlite3_bind_text(sqlite3_stmt* statement, int index, const char* text, int bytes, void (*destructor)(void*));
extern int sqlite3_bind_double(sqlite3_stmt* statement, int index, double value);
extern int sqlite3_step(sqlite3_stmt* statement);
extern int sqlite3_reset(sqlite3_stmt* statement);
extern int sqlite3_finalize(sqlite3_stmt* statement);
extern sqlite3_int64 sqlite3_column_int64(sqlite3_stmt* statement, int column);
extern double sqlite3_column_double(sqlite3_stmt* statement, int column);
extern const unsigned char* sqlite3_column_text(sqlite3_stmt* statement, int column);
extern int sqlite3_create_function(sqlite3* db, const char* name, int arguments, int encoding, void* user,
    void (*function)(sqlite3_context*, int, sqlite3_value**), void (*step)(sqlite3_context*, int, sqlite3_value**),
    void (*final)(sqlite3_context*));
extern int sqlite3_create_collation(sqlite3* db, const char* name, int encoding, void* user,
    int (*compare)(void*, int, const void*, int, const void*));
extern sqlite3_int64 sqlite3_value_int64(sqlite3_value* value);
extern void sqlite3_result_int64(sqlite3_context* context, sqlite3_int64 value);
extern void* sqlite3_user_data(sqlite3_context* context);
extern int sqlite3_blob_open(sqlite3* db, const char* schema, const char* table, const char* column, sqlite3_int64 row,
    int flags, sqlite3_blob** blob);
extern int sqlite3_blob_write(sqlite3_blob* blob, const void* data, int bytes, int offset);
extern int sqlite3_blob_read(sqlite3_blob* blob, void* data, int bytes, int offset);
extern int sqlite3_blob_close(sqlite3_blob* blob);
extern sqlite3_backup* sqlite3_backup_init(sqlite3* destination, const char* destination_name, sqlite3* source,
    const char* source_name);
extern int sqlite3_backup_step(sqlite3_backup* backup, int pages);
extern int sqlite3_backup_finish(sqlite3_backup* backup);
extern unsigned char* sqlite3_serialize(sqlite3* db, const char* schema, sqlite3_int64* size, unsigned int flags);
extern int sqlite3_deserialize(sqlite3* db, const char* schema, unsigned char* data, sqlite3_int64 size,
    sqlite3_int64 buffer_size, unsigned flags);
extern void sqlite3_free(void* memory);
extern const char* sqlite3_errmsg(sqlite3* db);
extern int printf(const char*, ...);
extern int unlink(const char*);

static const char* database = "alchemy_sqlite_fixture.db";
static int failed(sqlite3* db, const char* step) {
    printf("FAILED %s: %s\n", step, db ? sqlite3_errmsg(db) : "");
    return 1;
}

/* SQL function weight(x) = x * user scale. */
static void weight(sqlite3_context* context, int count, sqlite3_value** values) {
    (void)count;
    sqlite3_result_int64(context, sqlite3_value_int64(values[0]) * *(int*)sqlite3_user_data(context));
}
/* Collation "reverse": the reverse of byte order. */
static int collations;
static int reverse(void* user, int left_bytes, const void* left, int right_bytes, const void* right) {
    (void)user;
    ++collations;
    const unsigned char *a = left, *b = right;
    for (int index = 0; index < left_bytes && index < right_bytes; ++index)
        if (a[index] != b[index]) return a[index] < b[index] ? 1 : -1;
    return right_bytes - left_bytes;
}

static sqlite3_int64 scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* statement = 0;
    sqlite3_int64 value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, 0) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW)
        value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

int main(void) {
    sqlite3 *db = 0, *reader = 0, *copy = 0, *memory = 0;
    unlink(database);
    if (sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, 0) != SQLITE_OK)
        return failed(db, "open");
    sqlite3_stmt* statement = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL", -1, &statement, 0) != SQLITE_OK || sqlite3_step(statement) != SQLITE_ROW)
        return failed(db, "wal");
    printf("SQLite %s threadsafe %d journal %s\n", sqlite3_libversion(), sqlite3_threadsafe(), sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);

    if (sqlite3_exec(db, "CREATE TABLE item(id INTEGER PRIMARY KEY, name TEXT, size REAL, data BLOB);"
                         "CREATE VIRTUAL TABLE note USING fts5(body);", 0, 0, 0) != SQLITE_OK)
        return failed(db, "schema");
    if (sqlite3_exec(db, "BEGIN", 0, 0, 0) != SQLITE_OK) return failed(db, "begin");
    if (sqlite3_prepare_v2(db, "INSERT INTO item(id, name, size, data) VALUES(?1, ?2, ?3, zeroblob(16))", -1, &statement, 0) != SQLITE_OK)
        return failed(db, "prepare insert");
    const char* names[] = { "brush", "canvas", "layer", "palette", "gradient", "pattern" };
    for (int index = 0; index < 600; ++index) {
        sqlite3_bind_int64(statement, 1, index + 1);
        sqlite3_bind_text(statement, 2, names[index % 6], -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(statement, 3, index * 0.5);
        if (sqlite3_step(statement) != SQLITE_DONE) return failed(db, "insert");
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    if (sqlite3_exec(db, "INSERT INTO note(body) VALUES('soft round brush for sketching'), ('hard square canvas edge'),"
                         "('round watercolor brush'); COMMIT", 0, 0, 0) != SQLITE_OK)
        return failed(db, "fts insert");

    /* A second connection sees the committed rows through the WAL index. */
    if (sqlite3_open(database, &reader) != SQLITE_OK) return failed(reader, "open reader");
    printf("reader rows %lld sum %lld fts %lld json %lld\n", scalar(reader, "SELECT count(*) FROM item"),
        scalar(reader, "SELECT sum(size * 2) FROM item"),
        scalar(reader, "SELECT count(*) FROM note WHERE note MATCH 'round AND brush'"),
        scalar(reader, "SELECT json_extract('{\"a\":[1,2,{\"b\":42}]}', '$.a[2].b') + json_array_length('[1,2,3]')"));
    printf("math %lld\n", scalar(reader, "SELECT CAST(round(sqrt(2) * 1000) + floor(log10(12345)) + pow(2, 10) AS INTEGER)"));

    int scale = 3;
    if (sqlite3_create_function(db, "weight", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, &scale, weight, 0, 0) != SQLITE_OK ||
        sqlite3_create_collation(db, "reverse", SQLITE_UTF8, 0, reverse) != SQLITE_OK)
        return failed(db, "create function");
    if (sqlite3_prepare_v2(db, "SELECT name, sum(weight(id)) FROM item GROUP BY name ORDER BY name COLLATE reverse", -1, &statement, 0) != SQLITE_OK)
        return failed(db, "prepare grouped");
    printf("grouped:");
    while (sqlite3_step(statement) == SQLITE_ROW)
        printf(" %s=%lld", (const char*)sqlite3_column_text(statement, 0), sqlite3_column_int64(statement, 1));
    sqlite3_finalize(statement);
    printf(" (collation %s)\n", collations > 0 ? "called" : "not called");

    sqlite3_blob* blob = 0;
    const char pattern[16] = "Alchemy-blob-IO";
    char back[16] = { 0 };
    if (sqlite3_blob_open(db, "main", "item", "data", 77, 1, &blob) != SQLITE_OK ||
        sqlite3_blob_write(blob, pattern, 16, 0) != SQLITE_OK || sqlite3_blob_read(blob, back, 16, 0) != SQLITE_OK)
        return failed(db, "blob");
    sqlite3_blob_close(blob);
    printf("blob %s length %lld\n", back, scalar(db, "SELECT length(data) FROM item WHERE id = 77"));

    if (sqlite3_open(":memory:", &copy) != SQLITE_OK) return failed(copy, "open copy");
    sqlite3_backup* backup = sqlite3_backup_init(copy, "main", db, "main");
    if (!backup) return failed(copy, "backup init");
    int steps = 0;
    while (sqlite3_backup_step(backup, 2) == SQLITE_OK) ++steps;
    sqlite3_backup_finish(backup);
    sqlite3_int64 size = 0;
    unsigned char* image = sqlite3_serialize(copy, "main", &size, 0);
    /* The backup copied the WAL file-format bytes; a deserialized image must say rollback journal. */
    if (image && size > 100) image[18] = image[19] = 1;
    if (!image || sqlite3_open(":memory:", &memory) != SQLITE_OK ||
        sqlite3_deserialize(memory, "main", image, size, size, 1 | 2) != SQLITE_OK)   /* FREEONCLOSE | RESIZEABLE */
        return failed(memory, "deserialize");
    printf("backup steps %s rows %lld; deserialized rows %lld integrity %s\n", steps > 1 ? "several" : "one",
        scalar(copy, "SELECT count(*) FROM item"), scalar(memory, "SELECT count(*) FROM item WHERE name = 'layer'"),
        scalar(memory, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check = 'ok'") == 1 ? "ok" : "bad");

    sqlite3_close(memory);
    sqlite3_close(copy);
    sqlite3_close(reader);
    if (sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", 0, 0, 0) != SQLITE_OK) return failed(db, "checkpoint");
    sqlite3_close(db);
    unlink(database);
    return 0;
}
