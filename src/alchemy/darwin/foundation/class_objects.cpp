#include "alchemy/darwin/kit/completion/data/providers.hpp"

// Objective-C class objects as members of CF collections. QLab passes
// [NSSet setWithObjects:[NSArray class], [F53LicenseAccount class], nil] to
// +[NSKeyedUnarchiver unarchivedObjectOfClasses:fromData:error:], and
// -allObjects of such a set is a CF array. CFRetain/CFRelease and the CF
// collections know only object-store objects, so a class object (any
// registered class or metaclass, object_isClass) is adopted into the object
// store as an immortal record the first time CF meets it, as Foundation's
// class objects ignore retain and release. Message dispatch to classes is
// unchanged: the runtime resolves classes before the object store.
namespace alchemy::gui::completion_kit::data {

// The CoreFoundation-identity class symbols (NSCache, NSInputStream,
// NSInvocation, NSTimer) are exported once, by G2's runtime/cf_extras.cpp.
void install_class_objects(Support& support) {
    auto* sp = &support;
    support.cf.set_foreign_adopter([sp](std::uint64_t address) {
        if (!address || !(sp->call(objc_library, "_object_isClass", {address}) & 0xff)) return false;
        // objc_class: isa, superclass, cache, vtable, data (40 bytes).
        sp->process.objects().adopt(address, 40, sp->process.main_engine().read64(address), ObjectKind::instance, nullptr, true);
        return true;
    });
}

} // namespace alchemy::gui::completion_kit::data
