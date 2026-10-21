#include "cpp_exception_shared.hpp"
struct Cleanup {
    int* count;
    ~Cleanup() { ++*count; }
};
extern "C" void alchemy_throw(int* destructors) {
    Cleanup cleanup{destructors};
    throw AlchemyPayload{27};
}
extern "C" AlchemyBase* alchemy_object() {
    static AlchemyDerived object;
    return &object;
}
