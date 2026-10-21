/* Independently authored link-only placeholders; Alchemy replaces these. */
typedef unsigned long size_t;
typedef long ssize_t;
void *malloc(size_t n) { (void)n; return 0; }
void *calloc(size_t n, size_t s) { (void)n; (void)s; return 0; }
void *realloc(void *p, size_t n) { (void)p; (void)n; return 0; }
void free(void *p) { (void)p; }
void *memcpy(void *d, const void *s, size_t n) { (void)s; (void)n; return d; }
void *memmove(void *d, const void *s, size_t n) { (void)s; (void)n; return d; }
void *memset(void *d, int c, size_t n) { (void)c; (void)n; return d; }
int memcmp(const void *a, const void *b, size_t n) { (void)a; (void)b; (void)n; return 0; }
size_t strlen(const char *s) { (void)s; return 0; }
int strcmp(const char *a, const char *b) { (void)a; (void)b; return 0; }
char *strstr(const char *text, const char *needle) { (void)text; (void)needle; return 0; }
char *strcpy(char *d, const char *s) { (void)s; return d; }
char *getenv(const char *s) { (void)s; return 0; }
int setenv(const char *n, const char *v, int o) { (void)n; (void)v; (void)o; return -1; }
int unsetenv(const char *n) { (void)n; return -1; }
int open(const char *s, int f, ...) { (void)s; (void)f; return -1; }
int close(int d) { (void)d; return -1; }
ssize_t write(int d, const void *p, size_t n) { (void)d; (void)p; (void)n; return -1; }
ssize_t read(int d, void *p, size_t n) { (void)d; (void)p; (void)n; return -1; }
long lseek(int d, long o, int w) { (void)d; (void)o; (void)w; return -1; }
int stat(const char *n, void *s) { (void)n; (void)s; return -1; }
int fstat(int d, void *s) { (void)d; (void)s; return -1; }
int unlink(const char *p) { (void)p; return -1; }
int mkdir(const char *p, unsigned m) { (void)p; (void)m; return -1; }
int rmdir(const char *p) { (void)p; return -1; }
void *fopen(const char *p, const char *m) { (void)p; (void)m; return 0; }
int fclose(void *s) { (void)s; return -1; }
size_t fread(void *p, size_t s, size_t n, void *f) { (void)p; (void)s; (void)n; (void)f; return 0; }
size_t fwrite(const void *p, size_t s, size_t n, void *f) { (void)p; (void)s; (void)n; (void)f; return 0; }
int fseek(void *s, long o, int w) { (void)s; (void)o; (void)w; return -1; }
int fprintf(void *s, const char *f, ...) { (void)s; (void)f; return -1; }
int snprintf(char *p, size_t s, const char *f, ...) { (void)p; (void)s; (void)f; return -1; }
int clock_gettime(int c, void *t) { (void)c; (void)t; return -1; }
int getentropy(void *p, size_t n) { (void)p; (void)n; return -1; }
int atexit(void (*f)(void)) { (void)f; return -1; }
int *__error(void) { return 0; }
long sysconf(int n) { (void)n; return -1; }
int getpagesize(void) { return -1; }
int getpid(void) { return -1; }
int sysctl(const int *n, unsigned c, void *p, size_t *s, const void *v, size_t l) { (void)n; (void)c; (void)p; (void)s; (void)v; (void)l; return -1; }
int sysctlbyname(const char *n, void *p, size_t *s, const void *v, size_t l) { (void)n; (void)p; (void)s; (void)v; (void)l; return -1; }
void *mmap(void *a, size_t n, int p, int f, int d, long o) { (void)a; (void)n; (void)p; (void)f; (void)d; (void)o; return (void *)-1; }
int munmap(void *a, size_t n) { (void)a; (void)n; return -1; }
