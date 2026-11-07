/* Checks that a failing openat(O_DIRECTORY) on a plain file leaves ENOTDIR in errno, as libc++'s
   std::filesystem::remove_all relies on. Declarations are written here (Darwin values). */
int printf(const char *format, ...);
int *__error(void);
int open(const char *path, int flags, ...);
int openat(int fd, const char *path, int flags, ...);
int close(int fd);
int unlink(const char *path);
int mkdir(const char *path, unsigned short mode);
int rmdir(const char *path);
int unlinkat(int fd, const char *path, int flags);
#define O_RDONLY 0x0000
#define O_CREAT 0x0200
#define O_WRONLY 0x0001
#define O_NOFOLLOW 0x0100
#define O_DIRECTORY 0x100000
#define O_CLOEXEC 0x1000000
#define AT_FDCWD -2
#define ENOTDIR 20

int main(void) {
    mkdir("/tmp/alchemy-errno", 0755);
    close(open("/tmp/alchemy-errno/file", O_CREAT | O_WRONLY, 0644));
    int dir = open("/tmp/alchemy-errno", O_RDONLY | O_DIRECTORY);
    *__error() = 0;
    int fd = openat(dir, "file", O_CLOEXEC | O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    int e1 = *__error();
    *__error() = 0;
    int fd2 = openat(AT_FDCWD, "/tmp/alchemy-errno/file", O_RDONLY | O_DIRECTORY);
    int e2 = *__error();
    *__error() = 0;
    int fd3 = open("/tmp/alchemy-errno/file", O_RDONLY | O_DIRECTORY);
    int e3 = *__error();
    printf("openat(dirfd, file, O_DIRECTORY|O_NOFOLLOW) -> %d errno %d\n", fd, e1);
    printf("openat(AT_FDCWD, path, O_DIRECTORY)         -> %d errno %d\n", fd2, e2);
    printf("open(path, O_DIRECTORY)                     -> %d errno %d\n", fd3, e3);
    int u = unlinkat(dir, "file", 0);
    printf("unlinkat(dirfd, file, 0)                    -> %d errno %d\n", u, *__error());
    close(dir);
    rmdir("/tmp/alchemy-errno");
    return e1 == ENOTDIR && e2 == ENOTDIR && e3 == ENOTDIR && u == 0 ? 0 : 1;
}
