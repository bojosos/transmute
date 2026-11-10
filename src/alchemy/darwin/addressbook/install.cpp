#include "alchemy/darwin/addressbook/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <stdexcept>

// AddressBook: the user's address book is empty. Alchemy keeps no contacts
// and does not read the host's, so the shared address book has no people, no
// groups and no "me" record, and no record or multi-value exists for the
// record calls to read: given one, they stop explicitly. The kAB* property
// and record type names hold their own symbol names (the values are ours,
// unverified: Apple's public documentation does not give them). The calls that
// answer with an ABPropertyType stop too (its values are not in Apple's
// public documentation), as does the list of a record type's properties. Firefox (XUL) and VLC link the framework; LibreOffice's
// macOS address book driver (libmacabdrv1) calls the C API.
namespace alchemy::addressbook {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view address_book = "/System/Library/Frameworks/AddressBook.framework/Versions/A/AddressBook";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

[[noreturn]] void no_record(std::string_view call) {
    throw std::runtime_error("Alchemy AddressBook: " + std::string(call) + " was given a record, but the address book is empty");
}

void Provider::install() {
    ProcessContext::add_provider_library(std::string(address_book));
    const auto nothing = [](CpuState& s, GuestMemory&) { s.x[0] = 0; };
    const auto empty_array = [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); };

    // ---- Objective-C ----
    const auto book = cls(address_book, "ABAddressBook", "NSObject");
    singleton(book, "sharedAddressBook");
    method(book, "addressBook", [this, book](CpuState& s, GuestMemory&) { s.x[0] = objc.send(book, "sharedAddressBook").x0; }, true);
    method(book, "people", empty_array);
    method(book, "groups", empty_array);
    method(book, "me", nothing);
    method(book, "recordForUniqueId:", nothing);
    method(book, "recordsMatchingSearchElement:", empty_array);
    method(book, "hasUnsavedChanges", nothing, false, "B@:");
    for (const auto* name : {"ABRecord", "ABPerson", "ABGroup", "ABMultiValue", "ABMutableMultiValue", "ABSearchElement"}) {
        const auto parent = std::string_view(name) == "ABPerson" || std::string_view(name) == "ABGroup" ? "ABRecord"
                            : std::string_view(name) == "ABMutableMultiValue"                         ? "ABMultiValue"
                                                                                                       : "NSObject";
        static_cast<void>(cls(address_book, name, parent));
    }

    // The property and record type names: opaque keys over the empty store. Apple's public documentation
    // does not give their string values, so each holds its own name (ours, unverified).
    for (const auto* name : {"kABAddressProperty", "kABEmailProperty", "kABFirstNameProperty", "kABGroupNameProperty", "kABLastNameProperty",
                             "kABModificationDateProperty", "kABOrganizationProperty", "kABPersonRecordType", "kABGroupRecordType",
                             "kABPhoneProperty", "kABTitleProperty", "kABUIDProperty"})
        string(address_book, "_" + std::string(name), name);

    // ---- C API ----
    // ABAddressBookRef ABGetSharedAddressBook(void): the shared address book (not retained).
    fn(address_book, "_ABGetSharedAddressBook", [this, book](CpuState&, GuestMemory&) { return objc.send(book, "sharedAddressBook").x0; });
    // CFArrayRef ABCopyArrayOfAllPeople(ABAddressBookRef) / ABCopyArrayOfAllGroups: +1, empty.
    fn(address_book, "_ABCopyArrayOfAllPeople", [this](CpuState&, GuestMemory&) { return cf.array({}); });
    fn(address_book, "_ABCopyArrayOfAllGroups", [this](CpuState&, GuestMemory&) { return cf.array({}); });
    // ABPersonRef ABGetMe(ABAddressBookRef): no "me" record.
    fn(address_book, "_ABGetMe", [](CpuState&, GuestMemory&) { return Address{0}; });
    // CFStringRef ABCopyLocalizedPropertyOrLabel(CFStringRef): no localized names are known, so the name itself (+1).
    fn(address_book, "_ABCopyLocalizedPropertyOrLabel", [this](CpuState& s, GuestMemory&) { return s.x[0] ? retain(s.x[0]) : Address{0}; });
    // Record and multi-value calls: no record exists (NULL in, NULL or 0 out).
    fn(address_book, "_ABRecordCopyValue", [](CpuState& s, GuestMemory&) -> Address { if (s.x[0]) no_record("ABRecordCopyValue"); return 0; });
    fn(address_book, "_ABGroupCopyArrayOfAllMembers", [](CpuState& s, GuestMemory&) -> Address {
        if (s.x[0]) no_record("ABGroupCopyArrayOfAllMembers");
        return 0;
    });
    fn(address_book, "_ABMultiValueCount", [](CpuState& s, GuestMemory&) -> Address { if (s.x[0]) no_record("ABMultiValueCount"); return 0; });
    fn(address_book, "_ABMultiValueCopyValueAtIndex", [](CpuState& s, GuestMemory&) -> Address {
        if (s.x[0]) no_record("ABMultiValueCopyValueAtIndex");
        return 0;
    });
    fn(address_book, "_ABMultiValueCopyLabelAtIndex", [](CpuState& s, GuestMemory&) -> Address {
        if (s.x[0]) no_record("ABMultiValueCopyLabelAtIndex");
        return 0;
    });
    unsupported(address_book, "_ABTypeOfProperty", "ABPropertyType values are not publicly documented");
    unsupported(address_book, "_ABMultiValuePropertyType", "ABPropertyType values are not publicly documented");
    unsupported(address_book, "_ABCopyArrayOfPropertiesForRecordType", "a record type's full set of properties is not defined");
}

} // namespace

void install_addressbook(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::addressbook
