#pragma once

#include "alchemy/macho.hpp"
#include "alchemy/process_context.hpp"

#include <memory>
#include <span>

namespace alchemy {

// A process owns this loader until every guest worker has stopped. Dynamic
// loading releases its metadata lock before invoking any guest initializer.
class ModuleLoader {
public:
    ModuleLoader(ProcessContext& context, std::filesystem::path executable,
                 std::optional<std::uint64_t> load_base = {},
                 std::vector<std::filesystem::path> dylib_directories = {},
                 std::vector<std::filesystem::path> runtime_directories = {});
    ~ModuleLoader();
    ModuleLoader(const ModuleLoader&) = delete;
    ModuleLoader& operator=(const ModuleLoader&) = delete;
    [[nodiscard]] std::uint64_t load_main();
    // load_main() with the executable already parsed by the caller (its
    // source must be the loader's executable): the image is not read twice.
    [[nodiscard]] std::uint64_t load_main(MachOImage parsed);
    void initialize(std::span<const std::uint64_t> startup);
    [[nodiscard]] std::uint64_t open(std::string_view path, unsigned flags,
                                     std::uint64_t caller = 0);
    [[nodiscard]] std::optional<RuntimeExport> symbol(std::uint64_t handle,
                                                     std::string_view name,
                                                     std::uint64_t caller = 0);
    // Closing retires a handle reference; mappings and initialized code remain
    // resident until the process and every guest worker have stopped.
    [[nodiscard]] bool close(std::uint64_t handle);
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace alchemy
