#pragma once

#include "transmute/pe_image.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace transmute {

// Loads an executable and its sibling ARM64 DLL dependencies into one guest process.
class GuestProcess {
public:
    explicit GuestProcess(const std::filesystem::path& executable,
                          std::optional<std::uint64_t> load_base = std::nullopt,
                          std::vector<std::filesystem::path> dll_directories = {});

    [[nodiscard]] PeImage& executable() noexcept { return *modules_.front(); }
    [[nodiscard]] const std::vector<std::unique_ptr<PeImage>>& modules() const noexcept {
        return modules_;
    }
    [[nodiscard]] const std::vector<PeImage*>& attachment_order() const noexcept {
        return attachment_order_;
    }
    struct DynamicLoadPlan {
        std::vector<std::unique_ptr<PeImage>> modules;
        std::vector<PeImage*> attachment_order;
        PeImage* requested{};
    };
    [[nodiscard]] DynamicLoadPlan prepare_dynamic_load(
        const std::filesystem::path& requested,
        const std::vector<std::filesystem::path>& search_directories,
        bool include_requested_directory) const;
    void reserve_dynamic_modules(std::size_t count);
    void adopt_dynamic_load(DynamicLoadPlan&& plan);

private:
    std::vector<std::unique_ptr<PeImage>> modules_;
    std::vector<PeImage*> attachment_order_;
};

} // namespace transmute
