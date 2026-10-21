#ifndef ALCHEMY_SWIFT_STDIO_H
#define ALCHEMY_SWIFT_STDIO_H
/* POSIX.1-2008 additions used by the Swift runtime, layered over the project
   base profile. Independently authored; no SDK content. */
#include_next <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
ssize_t getline(char** line, size_t* capacity, FILE* stream);
ssize_t getdelim(char** line, size_t* capacity, int delimiter, FILE* stream);
int asprintf(char** result, const char* format, ...) __attribute__((format(printf, 2, 3)));
int vasprintf(char** result, const char* format, va_list arguments) __attribute__((format(printf, 2, 0)));
#ifdef __cplusplus
}
#endif
#endif
