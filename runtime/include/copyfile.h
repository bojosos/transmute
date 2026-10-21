#ifndef ALCHEMY_COPYFILE_H
#define ALCHEMY_COPYFILE_H
typedef void* copyfile_state_t;
typedef unsigned int copyfile_flags_t;
#define COPYFILE_DATA (1U << 3)
#ifdef __cplusplus
extern "C" {
#endif
copyfile_state_t copyfile_state_alloc(void);
int copyfile_state_free(copyfile_state_t);
int fcopyfile(int, int, copyfile_state_t, copyfile_flags_t);
#ifdef __cplusplus
}
#endif
#endif
