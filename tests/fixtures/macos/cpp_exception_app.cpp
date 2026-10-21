#include "cpp_exception_shared.hpp"
extern "C" int puts(const char*);
int main() {
    int destructors = 0;
    try {
        alchemy_throw(&destructors);
        return 1;
    } catch (const AlchemyPayload& payload) {
        if (payload.value != 27 || destructors != 1) return 2;
    } catch (...) { return 3; }
    auto* derived = dynamic_cast<AlchemyDerived*>(alchemy_object());
    if (!derived || derived->value != 42) return 4;
    puts("Guest C++ exception and RTTI: 27 42");
    return 0;
}
