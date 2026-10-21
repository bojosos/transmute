// Metal smoke test (owner: A5): the MSL contract's fixed resource model and
// the presence of the host Vulkan loader (reported, not required).
#include "alchemy/msl/compiler.hpp"

#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

int main() {
    namespace msl = alchemy::msl;
    if (msl::stage_descriptor_set(msl::Stage::vertex) != 0 || msl::stage_descriptor_set(msl::Stage::fragment) != 1 ||
        msl::stage_descriptor_set(msl::Stage::kernel) != 0 || msl::texture_binding_base != 16) {
        std::cerr << "MSL resource model changed\n";
        return 1;
    }
#ifdef _WIN32
    if (const auto module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        const bool entry = GetProcAddress(module, "vkGetInstanceProcAddr") != nullptr;
        std::cout << "vulkan-1.dll present" << (entry ? "" : " (without vkGetInstanceProcAddr)") << '\n';
        FreeLibrary(module);
    } else {
        std::cout << "vulkan-1.dll not found (Metal devices will be unavailable)\n";
    }
#endif
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
    std::cout << "Vulkan headers configured\n";
#endif
    std::cout << "Alchemy Metal smoke passed\n";
    return 0;
}
