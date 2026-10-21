#include <stdexcept>
#include <string>
extern "C" void alchemy_cpp_throw(int* cleaned) {
    struct Cleanup { int* value; ~Cleanup() { ++*value; } } cleanup{cleaned};
    throw std::runtime_error(std::string("cross-dylib C++ exception: ") + "27");
}
