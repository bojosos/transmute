// Foundation pieces moved from darwin/identities/framework_gaps.cpp
// (file_attribute_keys.cpp): NSFileModificationDate. Also the creation and
// attribute-modification date resource keys (core_foundation.cpp) and the
// uncaught exception handler (exceptions.cpp).
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/path.hpp"

#include <filesystem>
#include <string>

namespace {
using namespace fill_test;

constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";

const Case foundation_file_attribute_keys_case("foundation_file_attribute_keys", [](Fixture& f) {
    require(f.string_constant(foundation, "_NSFileModificationDate") == "NSFileModificationDate", "NSFileModificationDate");

    // A real directory's creation and attribute-change dates, under both names.
    constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    const auto folder = f.cf.file_url(alchemy::guest_path_string(std::filesystem::temp_directory_path()), true);
    const auto value = f.block(8);
    for (const auto* key : {"CreationDateKey", "AttributeModificationDateKey"}) {
        const auto name = f.engine.read64(f.symbol(foundation, std::string("_NSURL") + key));
        require(name == f.engine.read64(f.symbol(cf, std::string("_kCFURL") + key)), std::string("kCFURL") + key + " is NSURL" + key);
        f.engine.write64(value, 0);
        require(f.call(cf, "_CFURLCopyResourcePropertyForKey", {folder, name, value, 0}) == 1 && f.engine.read64(value) &&
                    f.call(cf, "_CFGetTypeID", {f.engine.read64(value)}) == f.call(cf, "_CFDateGetTypeID"), std::string(key) + " is a date");
    }

    // Volume capacities: the host volume's size and what this user may use (important and opportunistic usage
    // answer that same figure).
    const auto capacity = [&](const char* key) -> std::int64_t {
        f.engine.write64(value, 0);
        const auto name = f.engine.read64(f.symbol(cf, std::string("_kCFURL") + key));
        require(f.engine.read64(f.symbol(foundation, std::string("_NSURL") + key)) == name &&
                    f.engine.read64(f.symbol(cf, std::string("_NSURL") + key)) == name,
                std::string(key) + ": the same object as NSURL" + key + " under Foundation and CoreFoundation");
        require(f.call(cf, "_CFURLCopyResourcePropertyForKey", {folder, name, value, 0}) == 1 && f.engine.read64(value), std::string(key));
        return static_cast<std::int64_t>(*f.cf.number_value(f.engine.read64(value)));
    };
    const auto total = capacity("VolumeTotalCapacityKey"), available = capacity("VolumeAvailableCapacityKey");
    require(total > 0 && available > 0 && available <= total, "the volume's total and available capacity");
    require(capacity("VolumeAvailableCapacityForImportantUsageKey") > 0 && capacity("VolumeAvailableCapacityForOpportunisticUsageKey") > 0,
            "the important- and opportunistic-usage capacities");
    require(f.string_constant(foundation, "_NSPresentationIntentAttributeName") == "NSPresentationIntent", "NSPresentationIntentAttributeName");

    // NSSetUncaughtExceptionHandler / NSGetUncaughtExceptionHandler.
    const auto handler = f.block(16);
    f.call(foundation, "_NSSetUncaughtExceptionHandler", {handler});
    require(f.call(foundation, "_NSGetUncaughtExceptionHandler") == handler, "NSGetUncaughtExceptionHandler");
    f.call(foundation, "_NSSetUncaughtExceptionHandler", {0});
    require(f.call(foundation, "_NSGetUncaughtExceptionHandler") == 0, "NSSetUncaughtExceptionHandler(NULL)");
});

} // namespace
