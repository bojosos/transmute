/* C99 <fenv.h> for arm64 Darwin, used only to compile conformance suites.
 *
 * Libm's last open-source release has no arm64 fenv.h. The values below are
 * AArch64 architecture facts: exception flags are the FPSR cumulative bits
 * (IOC, DZC, OFC, UFC, IXC, IDC) and rounding modes are FPCR.RMode (bits 23:22).
 * fenv_t holds FPSR and FPCR as 64-bit words, FE_DFL_ENV is &_FE_DFL_ENV, and
 * fexcept_t is unsigned short, which is the Darwin arm64 profile that
 * src/alchemy/darwin/libsystem/misc/numeric_math.cpp implements. */
#ifndef __FENV_H__
#define __FENV_H__

#include <sys/cdefs.h>

typedef struct {
    unsigned long long __fpsr;
    unsigned long long __fpcr;
} fenv_t;
typedef unsigned short fexcept_t;

#define FE_INVALID 0x0001
#define FE_DIVBYZERO 0x0002
#define FE_OVERFLOW 0x0004
#define FE_UNDERFLOW 0x0008
#define FE_INEXACT 0x0010
#define FE_DENORMAL 0x0080
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT | FE_DENORMAL)

#define FE_TONEAREST 0x00000000
#define FE_UPWARD 0x00400000
#define FE_DOWNWARD 0x00800000
#define FE_TOWARDZERO 0x00C00000

__BEGIN_DECLS
extern const fenv_t _FE_DFL_ENV;
#define FE_DFL_ENV (&_FE_DFL_ENV)

int feclearexcept(int);
int fegetexceptflag(fexcept_t *, int);
int feraiseexcept(int);
int fesetexceptflag(const fexcept_t *, int);
int fetestexcept(int);
int fegetround(void);
int fesetround(int);
int fegetenv(fenv_t *);
int feholdexcept(fenv_t *);
int fesetenv(const fenv_t *);
int feupdateenv(const fenv_t *);
__END_DECLS

#endif
