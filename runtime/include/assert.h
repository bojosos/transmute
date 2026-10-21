#include "alchemy_c.h"
#undef assert
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : __assert_rtn(__func__, __FILE__, __LINE__, #expression))
#endif
