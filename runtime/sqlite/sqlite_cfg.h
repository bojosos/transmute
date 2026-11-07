/* Build configuration of the guest SQLite (/usr/lib/libsqlite3.dylib), read by the amalgamation
   through _HAVE_SQLITE_CONFIG_H. The project's own choices, close to the feature set macOS
   applications expect from the system library: full-text search, R*Tree, column metadata,
   pre-update hooks, unlock notification, the dbstat table and loadable extensions. File locking
   is the portable POSIX style (Apple's AFP and proxy locking styles are off). */
#ifndef ALCHEMY_SQLITE_CFG_H
#define ALCHEMY_SQLITE_CFG_H

#define SQLITE_THREADSAFE 1
#define SQLITE_ENABLE_LOCKING_STYLE 0
#define SQLITE_ENABLE_FTS3 1
#define SQLITE_ENABLE_FTS3_PARENTHESIS 1
#define SQLITE_ENABLE_FTS4 1
#define SQLITE_ENABLE_FTS5 1
#define SQLITE_ENABLE_RTREE 1
#define SQLITE_ENABLE_GEOPOLY 1
#define SQLITE_ENABLE_COLUMN_METADATA 1
#define SQLITE_ENABLE_DBSTAT_VTAB 1
#define SQLITE_ENABLE_UNLOCK_NOTIFY 1
#define SQLITE_ENABLE_PREUPDATE_HOOK 1
#define SQLITE_ENABLE_STMTVTAB 1
#define SQLITE_ENABLE_DESERIALIZE 1
#define SQLITE_ENABLE_MATH_FUNCTIONS 1
#define SQLITE_ENABLE_NORMALIZE 1
#define SQLITE_ENABLE_SNAPSHOT 1
#define SQLITE_ENABLE_API_ARMOR 1
#define SQLITE_DEFAULT_FOREIGN_KEYS 0
#define SQLITE_DEFAULT_WAL_SYNCHRONOUS 1
#define SQLITE_MAX_VARIABLE_NUMBER 500000

#define HAVE_USLEEP 1
#define HAVE_LOCALTIME_R 1
#define HAVE_GMTIME_R 1
#define HAVE_STRCHRNUL 0
#define HAVE_ISNAN 0
#define HAVE_MALLOC_USABLE_SIZE 0
#define SQLITE_WITHOUT_ZONEMALLOC 1

#endif
