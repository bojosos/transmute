#ifndef ALCHEMY_DIRENT_H
#define ALCHEMY_DIRENT_H
#include "sys/types.h"
typedef struct alchemy_directory DIR;
struct dirent { ino_t d_ino; unsigned long long d_seekoff; unsigned short d_reclen, d_namlen; unsigned char d_type; char d_name[1024]; };
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#ifdef __cplusplus
extern "C" {
#endif
DIR* opendir(const char*);
DIR* fdopendir(int);
struct dirent* readdir(DIR*);
int closedir(DIR*);
int dirfd(DIR*);
#ifdef __cplusplus
}
#endif
#endif
