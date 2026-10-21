#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/metal/gpu_identity.hpp"

#include <mutex>

// Metal on Vulkan built without the Vulkan-Headers (ALCHEMY_VULKAN_INCLUDE
// not found at configure time): nothing is installed, so
// MTLCreateSystemDefaultDevice keeps the headless answer (nil) of
// darwin_frameworks.cpp and Blender reports that Metal is unavailable.
namespace alchemy::gui {

void install_metal_gui(InstallContext&) {}
std::shared_ptr<void> prefetch_metal_device() { return nullptr; }
int seed_msl_corpus(const std::filesystem::path&, const std::filesystem::path&, const std::filesystem::path&, unsigned) {
    return 1;
}

} // namespace alchemy::gui

// No Vulkan headers, so no Vulkan enumeration: the IORegistry lists no GPU
// unless a test supplies one.
namespace alchemy::metal {
namespace {
std::mutex test_mutex;
std::vector<GpuIdentity> test_identities;
} // namespace

std::vector<GpuIdentity> gpu_identities() {
    std::lock_guard lock(test_mutex);
    return test_identities;
}
void set_gpu_identities_for_tests(std::vector<GpuIdentity> identities) {
    std::lock_guard lock(test_mutex);
    test_identities = std::move(identities);
}

} // namespace alchemy::metal
