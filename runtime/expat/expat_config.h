/* Alchemy build configuration for the guest ARM64 libexpat runtime.
 *
 * Project-authored (MIT). Consumed by scripts/build_alchemy_expat.ps1 in place
 * of the configure/CMake-generated header, for the Darwin LP64 profile that
 * runtime/include declares. Feature selection matches upstream expat's
 * defaults (DTD, general entities and namespaces on; attribute byte offsets
 * off; 1024 bytes of context), which is also the public API profile of the
 * libexpat.1 that macOS applications link against.
 */
#ifndef ALCHEMY_EXPAT_CONFIG_H
#define ALCHEMY_EXPAT_CONFIG_H 1

#define BYTEORDER 1234

/* Hash-salt entropy comes from libSystem's arc4random_buf, which Alchemy's
 * libSystem provider implements with the host system RNG. No /dev/urandom,
 * getrandom or getentropy fallback is compiled in. */
#define HAVE_ARC4RANDOM_BUF 1

#define HAVE_DLFCN_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

#define PACKAGE "expat"
#define PACKAGE_BUGREPORT "https://github.com/libexpat/libexpat/issues"
#define PACKAGE_NAME "expat"
#define PACKAGE_TARNAME "expat"
#define PACKAGE_URL ""

#define XML_CONTEXT_BYTES 1024
#define XML_DTD 1
#define XML_GE 1
#define XML_NS 1

/* runtime/include/math.h declares functions only; the C99 classification
 * macro xmlparse.c uses is supplied here as the compiler builtin. */
#ifndef isnan
#define isnan(value) __builtin_isnan(value)
#endif

#endif
