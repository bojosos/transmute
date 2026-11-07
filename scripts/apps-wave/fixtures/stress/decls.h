/* Declarations for the stress fixture. No SDK headers: the values are Darwin's documented ABI
   (the open-source xnu and Libc releases), written here by hand. */
typedef unsigned long size_t;
typedef long ssize_t;
typedef int pid_t;
typedef long long off_t;
typedef void *pthread_t;
typedef unsigned int sigset_t;

struct darwin_sigaction {   /* struct sigaction for sigaction(2) on Darwin */
    void (*handler)(int);
    sigset_t mask;
    int flags;
};

#define O_RDONLY 0x0000
#define O_RDWR 0x0002
#define O_CREAT 0x0200
#define O_TRUNC 0x0400
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x0001
#define MAP_PRIVATE 0x0002
#define MAP_ANON 0x1000
#define MAP_FAILED ((void *)-1)
#define SIGUSR1 30
#define SIGALRM 14

int printf(const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buffer, size_t size);
ssize_t write(int fd, const void *buffer, size_t size);
int unlink(const char *path);
int ftruncate(int fd, off_t length);
void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void *address, size_t length);
int pthread_create(pthread_t *thread, const void *attributes, void *(*start)(void *), void *argument);
int pthread_join(pthread_t thread, void **result);
int sigaction(int signal, const struct darwin_sigaction *action, struct darwin_sigaction *old);
int kill(pid_t pid, int signal);
pid_t getpid(void);
pid_t fork(void);
pid_t waitpid(pid_t pid, int *status, int options);
int posix_spawn(pid_t *pid, const char *path, const void *actions, const void *attributes, char *const argv[],
                char *const envp[]);
void _exit(int status);
unsigned int alarm(unsigned int seconds);
int pause(void);
char *getenv(const char *name);
extern char **environ;

/* libutil.a */
long util_sum_squares(long count);
unsigned util_checksum(const unsigned char *data, size_t size);
int util_format_count(char *buffer, size_t size, const char *label, long value);
