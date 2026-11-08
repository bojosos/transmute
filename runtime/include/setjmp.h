/* Non-local jumps with Darwin's arm64 buffer sizes (_JBLEN 48 ints; sigjmp_buf one more for the
 * saved signal mask), as in Apple's open-source Libc include/arm/_setjmp.h. */
#ifndef ALCHEMY_SETJMP_H
#define ALCHEMY_SETJMP_H
#ifdef __cplusplus
extern "C" {
#endif
#define _JBLEN ((14 + 8 + 2) * 2)
typedef int jmp_buf[_JBLEN];
typedef int sigjmp_buf[_JBLEN + 1];
int setjmp(jmp_buf environment) __attribute__((returns_twice));
void longjmp(jmp_buf environment, int value) __attribute__((noreturn));
int _setjmp(jmp_buf environment) __attribute__((returns_twice));
void _longjmp(jmp_buf environment, int value) __attribute__((noreturn));
int sigsetjmp(sigjmp_buf environment, int save_mask) __attribute__((returns_twice));
void siglongjmp(sigjmp_buf environment, int value) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
#endif
