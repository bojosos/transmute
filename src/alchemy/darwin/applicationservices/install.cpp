#include "alchemy/darwin/applicationservices/install.hpp"

#include "alchemy/process_context.hpp"

#include <optional>
#include <string>
#include <string_view>

// ApplicationServices, the umbrella framework: its identity, and lookups of
// symbols bound to the umbrella fall through to the CoreGraphics, CoreText,
// ImageIO, ColorSync and CoreServices providers it re-exports. A renderer plug-in's images
// first bound symbols through it (moved from darwin/identities/framework_gaps.cpp).
namespace alchemy::applicationservices {
namespace {

constexpr std::string_view application_services = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";
constexpr std::string_view reexported_by_application_services[] = {
    "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
    "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText",
    "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO",
    "/System/Library/Frameworks/ColorSync.framework/Versions/A/ColorSync",
    "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices"};

} // namespace

void install_applicationservices(gui::InstallContext& context) {
    auto& process = context.process;
    ProcessContext::add_provider_library(std::string(application_services));

    // ApplicationServices re-exports its subframeworks: a symbol bound to the
    // umbrella resolves through their providers.
    auto previous = process.base_resolver;
    process.base_resolver = [&process, previous](std::string_view library, std::string_view symbol) -> std::optional<std::uint64_t> {
        if (library == application_services) {
            for (const auto target : reexported_by_application_services)
                if (const auto found = process.resolve(target, symbol)) return found->address;
        }
        return previous ? previous(library, symbol) : std::nullopt;
    };
}

} // namespace alchemy::applicationservices
