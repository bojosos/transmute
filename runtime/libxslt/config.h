/* Alchemy build configuration for the guest ARM64 libxslt and libexslt runtimes.
 *
 * Project-authored (MIT). Consumed by scripts/build_alchemy_libxslt.ps1 in place
 * of the configure/CMake-generated config.h, for the Darwin LP64 profile that
 * runtime/include declares. The feature selection is in the generated
 * libxslt/xsltconfig.h and libexslt/exsltconfig.h (the script fills in
 * upstream's templates).
 */
#ifndef ALCHEMY_LIBXSLT_CONFIG_H
#define ALCHEMY_LIBXSLT_CONFIG_H 1

#define HAVE_CLOCK_GETTIME 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_INTTYPES_H 1
#define HAVE_LOCALE_H 1
#define HAVE_LIBPTHREAD
#define HAVE_PTHREAD_H
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_STAT 1
/* xsl:sort collation: newlocale and strxfrm_l from <xlocale.h>. */
#define HAVE_STRXFRM_L 1
#define HAVE_XLOCALE_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define MODULE_EXTENSION ".so"

#endif
