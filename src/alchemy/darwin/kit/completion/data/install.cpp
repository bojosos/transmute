#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/compression/install.hpp"
#include "alchemy/darwin/coreservices/install.hpp"
#include "alchemy/darwin/foundation/install.hpp"

// QLab gap group G1 (data): Foundation data and serialization (archivers, NSData/NSString/collections additions, scanners, predicates, decimal numbers, calendars, formatters, UTType).
// Owner and scope: docs/ALCHEMY_QLAB_GAPS.md, group G1. Each provider file in
// this directory adds one install_* function called from here (providers.hpp);
// cmake/darwin/kit_completion_data.cmake lists the sources and the group's tests.
namespace alchemy::gui::completion_kit {

void install_group_data(Support& support) {
    using namespace data;
    gui::install_foundation_data_classes(support);
    gui::install_coreservices_uttypes(support);
    gui::install_foundation_xml_urls_coding(support);
    alchemy::compression::install_compression(support);
    gui::install_foundation_archive_classes(support);
}

} // namespace alchemy::gui::completion_kit
