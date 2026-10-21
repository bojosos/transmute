#include "nl_types.h"
#include "locale.h"
#define CODESET 0
#ifdef __cplusplus
extern "C" {
#endif
char* nl_langinfo(nl_item);
char* nl_langinfo_l(nl_item, locale_t);
#ifdef __cplusplus
}
#endif
