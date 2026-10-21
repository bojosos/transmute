#include "alchemy/gui/value_provider.hpp"

// OpenDirectory constants (Chrome's images first imported them), cut from
// darwin/coreservices/services.cpp (install_services calls it in place).
namespace alchemy::gui::values {
namespace {

using fill::open_directory;

} // namespace

void Provider::install_open_directory() {
    string(open_directory, "_kODAttributeTypeStandardOnly", "dsAttributesStandardAll");
    string(open_directory, "_kODAttributeTypeUniqueID", "dsAttrTypeStandard:UniqueID");
    string(open_directory, "_kODRecordTypeGroups", "dsRecTypeStandard:Groups");
}

} // namespace alchemy::gui::values
