// GUI Objective-C contract smoke test (owner: A1): the host->guest register
// image and the public messaging API compile and lay out arguments by the
// Apple ARM64 rules.
#include "alchemy/objc_runtime.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        alchemy::CallRegs registers;
        registers.set_d(0, 1.5);
        registers.set_s(1, 2.0F);
        require(registers.uses_vectors(), "vector registers not recorded");
        // Apple ARM64 named stack arguments pack at natural alignment.
        registers.push_stack(std::uint8_t{1});
        registers.push_stack(std::uint16_t{2});
        registers.push_stack(std::uint64_t{3});
        require(registers.stack.size() == 16 && registers.stack[0] == 1 && registers.stack[2] == 2 &&
                    registers.stack[8] == 3, "named stack arguments are not naturally aligned");
        alchemy::CallRegs variadic;
        variadic.push_variadic(std::uint8_t{7});
        variadic.push_variadic(std::int32_t{-1});
        require(variadic.stack.size() == 16 && variadic.stack[8] == 0xff, "variadic arguments do not use 8-byte slots");
        alchemy::CallResult result{};
        result.v[0] = {0x3ff8000000000000ULL, 0};
        require(result.d(0) == 1.5, "CallResult double view");
        static_cast<void>(alchemy::ObjcRuntime::tracing());
        std::cout << "Alchemy GUI objc smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
