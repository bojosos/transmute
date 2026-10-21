// Interface of fxcorpus_dll.dll (guest DLL used by dll_user and dynload).
#pragma once

#include <stdint.h>

#ifdef FXCORPUS_DLL_BUILD
#define FXDLL_DATA extern
#else
#define FXDLL_DATA __declspec(dllimport) extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x, y;
} FxPoint;

typedef struct {
    long long values[6];
    double weight;
} FxBig;

typedef int (*FxCallback)(int value, void* context);

int fxdll_add(int a, int b);
int64_t fxdll_mul64(int64_t a, int64_t b);
double fxdll_mix(double d, float f, int i, long long l, float g);
int fxdll_sum_many(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11);
double fxdll_sum_fp_many(double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8,
                         double d9, double d10);
int fxdll_apply(FxCallback callback, void* context, int count);
const char* fxdll_name(void);
FxPoint fxdll_make_point(int x, int y);
FxBig fxdll_make_big(int seed);
int fxdll_vsum(int count, ...);
int fxdll_tls_get(void);
void fxdll_tls_set(int value);
int fxdll_initializer_ran(void);
int fxdll_by_ordinal(int value);

FXDLL_DATA int fxdll_counter;
FXDLL_DATA long fxdll_process_attach;
FXDLL_DATA long fxdll_thread_attach;
FXDLL_DATA long fxdll_thread_detach;

#ifdef __cplusplus
}
#endif
