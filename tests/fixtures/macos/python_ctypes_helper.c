/* Project-owned guest code. No host library or Apple SDK is used. */
struct alchemy_pair { int integer; double real; };

int alchemy_ctypes_value = 27;

long long alchemy_ctypes_add(long long left, long long right) {
    return left + right;
}

double alchemy_ctypes_scale(double value, double factor) {
    return value * factor;
}

struct alchemy_pair alchemy_ctypes_pair(struct alchemy_pair value) {
    value.integer += 27;
    value.real *= 2.0;
    return value;
}

int alchemy_ctypes_sum(const int* values, unsigned count) {
    int result = 0;
    for (unsigned index = 0; index < count; ++index) result += values[index];
    return result;
}

int alchemy_ctypes_callback(int (*callback)(int), int value) {
    return callback(value) + 27;
}

double alchemy_ctypes_float_callback(double (*callback)(double), double value) {
    return callback(value) + 0.5;
}
