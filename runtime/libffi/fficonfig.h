/* Build configuration of the guest libffi (/usr/lib/libffi.dylib) for arm64 macOS, written for
   scripts/build_alchemy_libffi.ps1 in place of the header libffi's configure generates. The values
   are those configure picks for aarch64-apple-darwin: closures through the trampoline table
   (FFI_EXEC_TRAMPOLINE_TABLE), long double the same as double, underscored symbols. */
#ifndef ALCHEMY_LIBFFI_FFICONFIG_H
#define ALCHEMY_LIBFFI_FFICONFIG_H

#define FFI_EXEC_TRAMPOLINE_TABLE 1
#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE 1
#define HAVE_INTTYPES_H 1
#define HAVE_MEMCPY 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1
#define SYMBOL_UNDERSCORE 1
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 8
#define SIZEOF_SIZE_T 8

#define PACKAGE "libffi"
#define PACKAGE_NAME "libffi"
#define PACKAGE_TARNAME "libffi"
#define PACKAGE_VERSION "3.8.0"
#define PACKAGE_STRING "libffi 3.8.0"
#define PACKAGE_BUGREPORT "http://github.com/libffi/libffi/issues"
#define PACKAGE_URL ""
#define VERSION "3.8.0"

#ifdef HAVE_HIDDEN_VISIBILITY_ATTRIBUTE
#ifdef LIBFFI_ASM
#ifdef __APPLE__
#define FFI_HIDDEN(name) .private_extern name
#else
#define FFI_HIDDEN(name) .hidden name
#endif
#else
#define FFI_HIDDEN __attribute__ ((visibility ("hidden")))
#endif
#else
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name)
#else
#define FFI_HIDDEN
#endif
#endif

#endif
