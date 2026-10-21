// stdio, low-level CRT and Win32 file I/O in the working directory.
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DIR_NAME "fx_io_dir"
#define DATA_SIZE 200000

static uint32_t fnv(const unsigned char* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int compare_names(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static void cleanup(void) {
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(DIR_NAME "\\*", &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            char path[MAX_PATH];
            snprintf(path, sizeof path, DIR_NAME "\\%s", fd.cFileName);
            DeleteFileA(path);
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryA(DIR_NAME);
}

int main(void) {
    cleanup();
    printf("mkdir=%d\n", CreateDirectoryA(DIR_NAME, NULL) != 0);

    unsigned char* data = (unsigned char*)malloc(DATA_SIZE);
    for (int i = 0; i < DATA_SIZE; i++) data[i] = (unsigned char)((i * 7) ^ (i >> 8));
    printf("data fnv=%08x\n", fnv(data, DATA_SIZE));

    // stdio binary
    FILE* f = fopen(DIR_NAME "\\data.bin", "wb");
    size_t written = fwrite(data, 1, DATA_SIZE, f);
    printf("fwrite=%zu fclose=%d\n", written, fclose(f));
    struct _stat64 st;
    int stat_result = _stat64(DIR_NAME "\\data.bin", &st);
    printf("stat=%d size=%lld\n", stat_result, (long long)st.st_size);

    f = fopen(DIR_NAME "\\data.bin", "rb");
    unsigned char chunk[16];
    fseek(f, 5000, SEEK_SET);
    size_t got = fread(chunk, 1, sizeof chunk, f);
    printf("fread=%zu ftell=%ld bytes=", got, ftell(f));
    for (size_t i = 0; i < got; i++) printf("%02x", chunk[i]);
    printf("\n");
    fseek(f, 0, SEEK_END);
    printf("end ftell=%ld\n", ftell(f));
    fseek(f, -10, SEEK_END);
    int c = fgetc(f);
    printf("fgetc at end-10=%d feof=%d\n", c, feof(f));
    fseek(f, 0, SEEK_END);
    c = fgetc(f);
    printf("fgetc at end=%d feof=%d\n", c, feof(f) != 0);
    rewind(f);
    unsigned char* back = (unsigned char*)malloc(DATA_SIZE);
    got = fread(back, 1, DATA_SIZE, f);
    printf("full read=%zu equal=%d\n", got, memcmp(back, data, DATA_SIZE) == 0);
    fclose(f);

    // stdio text mode (\n <-> \r\n) and formatted I/O
    f = fopen(DIR_NAME "\\text.txt", "w");
    fprintf(f, "line one\nline two %d %.3f\n", 42, 2.5);
    fputs("third\n", f);
    fclose(f);
    f = fopen(DIR_NAME "\\text.txt", "rb");
    fseek(f, 0, SEEK_END);
    printf("text file bytes=%ld\n", ftell(f));
    fclose(f);
    f = fopen(DIR_NAME "\\text.txt", "r");
    char line[64];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        printf("fgets=[%s]\n", line);
    }
    fclose(f);
    f = fopen(DIR_NAME "\\text.txt", "r");
    char w1[16], w2[16], w3[16];
    int n = 0;
    double d = 0;
    int fields = fscanf(f, "%15s %15s %15s %15s %d %lf", w1, w2, w1, w3, &n, &d);
    printf("fscanf fields=%d %s %d %.3f\n", fields, w3, n, d);
    fclose(f);

    f = fopen(DIR_NAME "\\text.txt", "a+");
    fputs("appended\n", f);
    fseek(f, 0, SEEK_SET);
    int lines = 0;
    while (fgets(line, sizeof line, f)) lines++;
    printf("append lines=%d\n", lines);
    fclose(f);

    // low-level CRT
    int fd = _open(DIR_NAME "\\low.bin", _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    printf("_write=%d\n", _write(fd, data, 4096));
    printf("_lseek=%ld\n", _lseek(fd, 1000, SEEK_SET));
    int read_count = _read(fd, chunk, 16);
    printf("_read=%d match=%d\n", read_count, memcmp(chunk, data + 1000, 16) == 0);
    long file_length = _filelength(fd);
    printf("_filelength=%ld _close=%d\n", file_length, _close(fd));

    // Win32
    HANDLE h = CreateFileA(DIR_NAME "\\win32.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD io = 0;
    BOOL ok = WriteFile(h, data, DATA_SIZE, &io, NULL);
    printf("WriteFile ok=%d bytes=%lu\n", ok != 0, io);
    LARGE_INTEGER size, pos;
    GetFileSizeEx(h, &size);
    pos.QuadPart = 123456;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    ok = ReadFile(h, chunk, 16, &io, NULL);
    printf("GetFileSizeEx=%lld ReadFile ok=%d bytes=%lu match=%d\n", size.QuadPart, ok != 0, io,
           memcmp(chunk, data + 123456, 16) == 0);
    pos.QuadPart = 0;
    SetFilePointerEx(h, pos, NULL, FILE_END);
    WriteFile(h, "tail", 4, &io, NULL);
    FlushFileBuffers(h);
    GetFileSizeEx(h, &size);
    printf("after append size=%lld\n", size.QuadPart);

    HANDLE mapping = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    const unsigned char* view = (const unsigned char*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    printf("mapped fnv=%08x tail=%.4s\n", fnv(view, DATA_SIZE), (const char*)view + DATA_SIZE);
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    CloseHandle(h);

    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    h = CreateFileA(DIR_NAME "\\win32.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ov.Offset = 77;
    ok = ReadFile(h, chunk, 8, &io, &ov);
    printf("ReadFile at offset ok=%d bytes=%lu match=%d\n", ok != 0, io, memcmp(chunk, data + 77, 8) == 0);
    CloseHandle(h);

    printf("MoveFileA=%d\n", MoveFileA(DIR_NAME "\\win32.bin", DIR_NAME "\\renamed.bin") != 0);
    printf("CopyFileA=%d\n", CopyFileA(DIR_NAME "\\renamed.bin", DIR_NAME "\\copy.bin", TRUE) != 0);
    BOOL copied_again = CopyFileA(DIR_NAME "\\renamed.bin", DIR_NAME "\\copy.bin", TRUE);
    printf("CopyFileA exists=%d error=%lu\n", copied_again != 0, GetLastError());
    int renamed = rename(DIR_NAME "\\low.bin", DIR_NAME "\\low2.bin");
    printf("rename=%d remove=%d\n", renamed, remove(DIR_NAME "\\text.txt"));

    char names[16][MAX_PATH];
    int count = 0;
    WIN32_FIND_DATAA find_data;
    HANDLE find = FindFirstFileA(DIR_NAME "\\*", &find_data);
    do {
        if (find_data.cFileName[0] == '.') continue;
        if (count < 16) {
            snprintf(names[count], MAX_PATH, "%s:%lu", find_data.cFileName, find_data.nFileSizeLow);
            count++;
        }
    } while (FindNextFileA(find, &find_data));
    printf("FindNextFile end error=%lu\n", GetLastError());
    FindClose(find);
    qsort(names, (size_t)count, sizeof names[0], compare_names);
    for (int i = 0; i < count; i++) printf("entry %s\n", names[i]);

    DWORD attrs = GetFileAttributesA(DIR_NAME "\\missing.bin");
    printf("missing attrs=%lx error=%lu\n", attrs, GetLastError());
    h = CreateFileA(DIR_NAME "\\missing.bin", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    printf("open missing invalid=%d error=%lu\n", h == INVALID_HANDLE_VALUE, GetLastError());
    f = fopen(DIR_NAME "\\missing.bin", "rb");
    printf("fopen missing null=%d errno=%d\n", f == NULL, errno);

    cleanup();
    printf("cleanup exists=%d\n", GetFileAttributesA(DIR_NAME) != INVALID_FILE_ATTRIBUTES);
    free(back);
    free(data);
    fflush(stdout);
    return 0;
}
