#ifndef ALCHEMY_C_H
#define ALCHEMY_C_H
/* Independently authored Darwin LP64 declarations. No SDK implementation. */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#ifdef __cplusplus
#define ALCHEMY_NOEXCEPT noexcept
extern "C" {
#else
#define ALCHEMY_NOEXCEPT
#endif
typedef long ssize_t;
typedef long off_t;
typedef long time_t;
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned short mode_t;
typedef unsigned long clock_t;
struct timespec { time_t tv_sec; long tv_nsec; };
struct timeval { long tv_sec; int tv_usec; };
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; char* tm_zone; };
typedef long long fpos_t;
struct __sbuf { unsigned char* _base; int _size; };
struct __sFILEX;
/* Darwin's public stdio FILE (struct __sFILE in the open-source Libc's
   stdio.h, from BSD stdio). The name matters beyond C: Swift mangles it into
   the Darwin overlay's stdin/stdout/stderr ($s6Darwin6stderrSpySo7__sFILEVGvg). */
typedef struct __sFILE {
    unsigned char* _p; int _r; int _w; short _flags; short _file; struct __sbuf _bf; int _lbfsize;
    void* _cookie; int (*_close)(void*); int (*_read)(void*, char*, int); fpos_t (*_seek)(void*, fpos_t, int);
    int (*_write)(void*, const char*, int); struct __sbuf _ub; struct __sFILEX* _extra; int _ur;
    unsigned char _ubuf[3]; unsigned char _nbuf[1]; struct __sbuf _lb; int _blksize; fpos_t _offset;
} FILE;
/* The LP64 layout: guest code that uses stdio's inline accessors reads these fields. */
#ifdef __cplusplus
#define ALCHEMY_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define ALCHEMY_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif
ALCHEMY_STATIC_ASSERT(sizeof(struct __sbuf) == 16, "struct __sbuf size");
ALCHEMY_STATIC_ASSERT(sizeof(FILE) == 152, "FILE size");
ALCHEMY_STATIC_ASSERT(offsetof(FILE, _r) == 8 && offsetof(FILE, _w) == 12 && offsetof(FILE, _flags) == 16 &&
    offsetof(FILE, _file) == 18 && offsetof(FILE, _bf) == 24 && offsetof(FILE, _lbfsize) == 40, "FILE buffer fields");
ALCHEMY_STATIC_ASSERT(offsetof(FILE, _cookie) == 48 && offsetof(FILE, _close) == 56 && offsetof(FILE, _read) == 64 &&
    offsetof(FILE, _seek) == 72 && offsetof(FILE, _write) == 80, "FILE function fields");
ALCHEMY_STATIC_ASSERT(offsetof(FILE, _ub) == 88 && offsetof(FILE, _extra) == 104 && offsetof(FILE, _ur) == 112 &&
    offsetof(FILE, _ubuf) == 116 && offsetof(FILE, _nbuf) == 119 && offsetof(FILE, _lb) == 120 &&
    offsetof(FILE, _blksize) == 136 && offsetof(FILE, _offset) == 144, "FILE ungetc and line-buffer fields");
#undef ALCHEMY_STATIC_ASSERT
extern FILE* __stdinp;
extern FILE* __stdoutp;
extern FILE* __stderrp;
int* __error(void);
void* malloc(size_t);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void free(void*);
int posix_memalign(void**, size_t, size_t);
void* aligned_alloc(size_t, size_t);
void abort(void) __attribute__((noreturn));
void exit(int) __attribute__((noreturn));
int atexit(void (*)(void));
char* getenv(const char*);
int setenv(const char*, const char*, int);
int unsetenv(const char*);
long strtol(const char*, char**, int);
unsigned long strtoul(const char*, char**, int);
long long strtoll(const char*, char**, int);
unsigned long long strtoull(const char*, char**, int);
float strtof(const char*, char**);
double strtod(const char*, char**);
long double strtold(const char*, char**);
int atoi(const char*);
long atol(const char*);
long long atoll(const char*);
int abs(int);
long labs(long);
long long llabs(long long);
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
div_t div(int, int);
ldiv_t ldiv(long, long);
lldiv_t lldiv(long long, long long);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
uint32_t arc4random(void);
void arc4random_buf(void*, size_t);
uint32_t arc4random_uniform(uint32_t);
void* bsearch(const void*, const void*, size_t, size_t, int (*)(const void*, const void*));
void* memcpy(void*, const void*, size_t);
void* memmove(void*, const void*, size_t);
void* memset(void*, int, size_t);
int memcmp(const void*, const void*, size_t);
void* memchr(const void*, int, size_t);
size_t strlen(const char*);
size_t strnlen(const char*, size_t);
char* strcpy(char*, const char*);
char* strncpy(char*, const char*, size_t);
char* strcat(char*, const char*);
char* strncat(char*, const char*, size_t);
int strcmp(const char*, const char*);
int strncmp(const char*, const char*, size_t);
char* strchr(const char*, int);
char* strrchr(const char*, int);
char* strstr(const char*, const char*);
char* strpbrk(const char*, const char*);
size_t strspn(const char*, const char*);
size_t strcspn(const char*, const char*);
char* strtok(char*, const char*);
char* strdup(const char*);
char* strerror(int);
int strerror_r(int, char*, size_t);
int strcoll(const char*, const char*);
size_t strxfrm(char*, const char*, size_t);
FILE* fopen(const char*, const char*);
int vdprintf(int, const char*, va_list);
int dprintf(int, const char*, ...);
FILE* freopen(const char*, const char*, FILE*);
FILE* fdopen(int, const char*);
int fclose(FILE*);
int fflush(FILE*);
int feof(FILE*);
int ferror(FILE*);
void clearerr(FILE*);
int fseek(FILE*, long, int);
int fseeko(FILE*, off_t, int);
long ftell(FILE*);
off_t ftello(FILE*);
void rewind(FILE*);
int fileno(FILE*);
size_t fread(void*, size_t, size_t, FILE*);
size_t fwrite(const void*, size_t, size_t, FILE*);
int fgetc(FILE*);
int fputc(int, FILE*);
int getc(FILE*);
int putc(int, FILE*);
void flockfile(FILE*);
void funlockfile(FILE*);
int ftrylockfile(FILE*);
char* fgets(char*, int, FILE*);
int fputs(const char*, FILE*);
int getchar(void);
int putchar(int);
int puts(const char*);
int ungetc(int, FILE*);
int printf(const char*, ...);
int fprintf(FILE*, const char*, ...);
int sprintf(char*, const char*, ...);
int snprintf(char*, size_t, const char*, ...);
int vprintf(const char*, va_list);
int vfprintf(FILE*, const char*, va_list);
int vsprintf(char*, const char*, va_list);
int vsnprintf(char*, size_t, const char*, va_list);
int scanf(const char*, ...);
int fscanf(FILE*, const char*, ...);
int sscanf(const char*, const char*, ...);
int vscanf(const char*, va_list);
int vfscanf(FILE*, const char*, va_list);
int vsscanf(const char*, const char*, va_list);
int remove(const char*);
int rename(const char*, const char*);
FILE* tmpfile(void);
char* tmpnam(char*);
int setvbuf(FILE*, char*, int, size_t);
void setbuf(FILE*, char*);
void perror(const char*);
time_t time(time_t*);
clock_t clock(void);
double difftime(time_t, time_t);
time_t mktime(struct tm*);
struct tm* gmtime(const time_t*);
struct tm* localtime(const time_t*);
struct tm* gmtime_r(const time_t*, struct tm*);
struct tm* localtime_r(const time_t*, struct tm*);
char* asctime(const struct tm*);
char* ctime(const time_t*);
size_t strftime(char*, size_t, const char*, const struct tm*);
int clock_gettime(int, struct timespec*);
int clock_getres(int, struct timespec*);
uint64_t clock_gettime_nsec_np(int);
int timespec_get(struct timespec*, int);
int nanosleep(const struct timespec*, struct timespec*);
int gettimeofday(struct timeval*, void*);
int sched_yield(void);
int close(int);
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int chdir(const char*);
char* getcwd(char*, size_t);
int ftruncate(int, off_t);
int truncate(const char*, off_t);
int link(const char*, const char*);
int symlink(const char*, const char*);
ssize_t readlink(const char*, char*, size_t);
char* realpath(const char*, char*);
int unlink(const char*);
int rmdir(const char*);
ssize_t read(int, void*, size_t);
ssize_t write(int, const void*, size_t);
off_t lseek(int, off_t, int);
long sysconf(int);
int isatty(int);
unsigned sleep(unsigned);
int usleep(unsigned);
void __assert_rtn(const char*, const char*, int, const char*) __attribute__((noreturn));
typedef int errno_t;
typedef size_t rsize_t;
errno_t memset_s(void*, rsize_t, int, rsize_t);
#ifdef __cplusplus
}
#endif
#define errno (*__error())
#define stdin __stdinp
#define stdout __stdoutp
#define stderr __stderrp
#define EOF (-1)
#define BUFSIZ 1024
#define FOPEN_MAX 20
#define FILENAME_MAX 1024
#define L_tmpnam 1024
#define TMP_MAX 308915776
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
#define CLOCKS_PER_SEC 1000000
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 6
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_MONOTONIC_RAW_APPROX 5
#define CLOCK_UPTIME_RAW 8
#define CLOCK_UPTIME_RAW_APPROX 9
#define CLOCK_PROCESS_CPUTIME_ID 12
#define CLOCK_THREAD_CPUTIME_ID 16
#define TIME_UTC 1
#define _SC_NPROCESSORS_ONLN 58
#define EDOM 33
#define ERANGE 34
#define EINVAL 22
#define ENOMEM 12
#define EAGAIN 35
#define EBUSY 16
#define EDEADLK 11
#define ETIMEDOUT 60
#define EINTR 4
#define ENOSYS 78
#define ELAST 106
#endif
