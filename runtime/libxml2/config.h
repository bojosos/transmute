/* Alchemy build configuration for the guest ARM64 libxml2 runtime.
 *
 * Project-authored (MIT). Consumed by scripts/build_alchemy_libxml2.ps1 in place
 * of the configure/CMake-generated config.h, for the Darwin LP64 profile that
 * runtime/include declares. The feature selection itself is in the generated
 * libxml/xmlversion.h (the script fills in upstream's xmlversion.h.in).
 */
#ifndef ALCHEMY_LIBXML2_CONFIG_H
#define ALCHEMY_LIBXML2_CONFIG_H 1

/* Hash and dictionary seeds come from getentropy (sys/random.h), which
 * Alchemy's libSystem provider fills from the host random source. */
#define HAVE_DECL_GETENTROPY 1
/* glob is used only by the xmllint program, which is not built. */
#define HAVE_DECL_GLOB 0
/* Documents are read with read(2); no mmap path. */
#define HAVE_DECL_MMAP 0
#define HAVE_FUNC_ATTRIBUTE_DESTRUCTOR 1
/* xmlModuleOpen uses dlopen/dlsym/dlclose from libSystem. */
#define HAVE_DLOPEN 1
#define HAVE_STDINT_H 1
#define XML_SYSCONFDIR "/etc"

#endif
