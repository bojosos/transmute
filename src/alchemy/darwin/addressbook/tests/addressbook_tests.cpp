// AddressBook (src/alchemy/darwin/addressbook/): the identity, and the empty address book.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/AddressBook.framework/Versions/A/AddressBook"
#define CF "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"

const Case addressbook_case("addressbook", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "AddressBook identity");
    const auto shared = f.call(LIBRARY, "_ABGetSharedAddressBook");
    require(shared != 0 && shared == f.send(f.cls("ABAddressBook"), "sharedAddressBook"), "one shared address book");
    const auto people = f.call(LIBRARY, "_ABCopyArrayOfAllPeople", {shared});
    const auto groups = f.call(LIBRARY, "_ABCopyArrayOfAllGroups", {shared});
    require(people && groups && f.cf.array_values(people).empty() && f.cf.array_values(groups).empty(), "no people or groups");
    static_cast<void>(f.call(CF, "_CFRelease", {people}));
    static_cast<void>(f.call(CF, "_CFRelease", {groups}));
    require(f.cf.array_values(f.send(shared, "people")).empty() && f.send(shared, "me") == 0, "no people, no me record");
    require(f.call(LIBRARY, "_ABGetMe", {shared}) == 0, "ABGetMe is NULL");
    const auto name = f.cf.string("First");
    const auto localized = f.call(LIBRARY, "_ABCopyLocalizedPropertyOrLabel", {name});
    require(localized && f.cf.utf8(localized) == "First", "a property name stays as it is");
    static_cast<void>(f.call(CF, "_CFRelease", {localized}));
    static_cast<void>(f.call(CF, "_CFRelease", {name}));
    require(f.call(LIBRARY, "_ABRecordCopyValue", {0, 0}) == 0 && f.call(LIBRARY, "_ABMultiValueCount", {0}) == 0, "NULL records read nothing");
    require(f.string_constant(LIBRARY, "_kABFirstNameProperty") == "kABFirstNameProperty" &&
                f.string_constant(LIBRARY, "_kABPersonRecordType") == "kABPersonRecordType", "the key names (ours)");
    bool stopped = false;
    try { static_cast<void>(f.call(LIBRARY, "_ABRecordCopyValue", {shared, 0})); } catch (const std::exception&) { stopped = true; }
    require(stopped, "a record that cannot exist stops explicitly");
});

#undef LIBRARY
#undef CF
} // namespace
