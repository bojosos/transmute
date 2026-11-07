#include "decls.h"

long util_sum_squares(long count) {
    long total = 0;
    for (long i = 1; i <= count; i++) total += i * i;
    return total;
}

unsigned util_checksum(const unsigned char *data, size_t size) {
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < size; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}
