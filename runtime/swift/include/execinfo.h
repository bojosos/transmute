#ifndef ALCHEMY_EXECINFO_H
#define ALCHEMY_EXECINFO_H
/* Published backtrace declarations. Independently authored; no SDK
   content. */
#ifdef __cplusplus
extern "C" {
#endif
int backtrace(void** array, int size);
char** backtrace_symbols(void* const* array, int size);
void backtrace_symbols_fd(void* const* array, int size, int fd);
#ifdef __cplusplus
}
#endif
#endif
