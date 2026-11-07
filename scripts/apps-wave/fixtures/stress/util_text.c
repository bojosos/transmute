#include "decls.h"

int util_format_count(char *buffer, size_t size, const char *label, long value) {
    return snprintf(buffer, size, "%s=%ld", label, value);
}
