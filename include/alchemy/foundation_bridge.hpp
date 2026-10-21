#pragma once

#include "alchemy/core_foundation.hpp"
#include "alchemy/objc_runtime.hpp"

namespace alchemy {
namespace detail {
// Foundation classes beyond the core bridge (src/alchemy/darwin/foundation/foundation_extra.cpp).
[[nodiscard]] std::shared_ptr<void> install_foundation_extra(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf);
}
class FoundationBridge {
public:
    FoundationBridge(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf);
    ~FoundationBridge();
    FoundationBridge(const FoundationBridge&) = delete;
    FoundationBridge& operator=(const FoundationBridge&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace alchemy
