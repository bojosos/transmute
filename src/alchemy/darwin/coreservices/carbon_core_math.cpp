#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

#include <cmath>

// CoreServices CarbonCore fp.h exports: the libSystem implementations, or
// the pure functions here when libSystem does not provide them.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

// <fenv.h> on arm64 Darwin: the FE_* bits are the FPSR cumulative flags.
constexpr std::uint32_t all_exceptions = 0x9f;

class CarbonCoreMath final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // CarbonCore's fp.h exports: the libSystem implementations, or the
        // pure functions here when libSystem does not provide them.
        const auto alias = [&](std::string symbol, transmute::GuestCallHandler fallback) {
            if (const auto found = context.resolve(libsystem, symbol)) context.install_export(std::string(core_services), symbol, *found);
            else function(core_services, std::move(symbol), std::move(fallback));
        };
        alias("_exp2", [](CpuState& s, GuestMemory&) { return_real(s, std::exp2(real(s, 0))); });
        alias("_log2", [](CpuState& s, GuestMemory&) { return_real(s, std::log2(real(s, 0))); });
        alias("_modff", [](CpuState& s, GuestMemory& m) {
            float whole{};
            const auto fraction = std::modf(single(s, 0), &whole);
            m.write32(s.x[0], std::bit_cast<std::uint32_t>(whole));
            return_single(s, fraction);
        });
        alias("_nextafterf", [](CpuState& s, GuestMemory&) { return_single(s, std::nextafter(single(s, 0), single(s, 1))); });
        alias("_remquo", [](CpuState& s, GuestMemory& m) {
            int quotient{};
            const auto value = std::remquo(real(s, 0), real(s, 1), &quotient);
            m.write32(s.x[0], static_cast<std::uint32_t>(quotient));
            return_real(s, value);
        });
        alias("_feclearexcept", [](CpuState& s, GuestMemory&) { s.fpsr &= ~(static_cast<std::uint32_t>(s.x[0]) & all_exceptions); s.x[0] = 0; });
        alias("_fetestexcept", [](CpuState& s, GuestMemory&) { s.x[0] = s.fpsr & static_cast<std::uint32_t>(s.x[0]) & all_exceptions; });
    }
};

} // namespace

void install_coreservices_carbon_core_math(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<CarbonCoreMath>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
