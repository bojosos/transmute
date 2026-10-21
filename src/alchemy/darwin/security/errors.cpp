#include "alchemy/darwin/security/security_internal.hpp"

#include <array>

// NSError and its toll-free CFError functions (Foundation / CoreFoundation
// identities). Alchemy's Foundation had no error objects; Security
// (CFErrorRef out parameters) and URL loading (NSURLErrorDomain completion
// errors) need them. Everything here is installed only when no other provider
// exported it first, so a later Foundation owner can take it over.
namespace alchemy::gui::security {
namespace {

struct ErrorState final : ObjectPayload {
    Address domain{};       // +1 CFString
    std::int64_t code{};
    Address user_info{};    // +1 CFDictionary or 0
};

constexpr std::string_view description_key = "NSLocalizedDescription";

struct Errors {
    std::shared_ptr<Bridge> b;
    std::map<std::string, Address, std::less<>> constants;   // interned constant strings

    Address constant_string(std::string_view value) {
        if (const auto found = constants.find(value); found != constants.end()) return found->second;
        const auto object = b->cf.constant_string(value);
        constants.emplace(std::string(value), object);
        return object;
    }
    void export_constant(std::string_view library, std::string_view symbol, std::string_view value) {
        if (b->exported(library, symbol)) return;
        b->export_value(library, symbol, constant_string(value));
    }

    std::shared_ptr<ErrorState> state(Address object) {
        auto value = b->objc.find_host_state<ErrorState>(object);
        if (!value) fail("NSError object is not initialized");
        return value;
    }
    // Builds the receiver's state (domain and userInfo are retained/copied).
    void initialize(Address object, Address domain, std::int64_t code, Address user_info) {
        if (!domain || !b->is_string(domain)) fail("NSError requires a domain string");
        auto value = std::make_shared<ErrorState>();
        value->domain = b->cf.copy(domain);
        value->code = code;
        try {
            if (user_info) {
                if (!b->is_dictionary(user_info)) fail("NSError userInfo is not a dictionary");
                value->user_info = b->cf.copy(user_info);
            }
        } catch (...) { b->release(value->domain); throw; }
        const auto created = b->objc.host_state<ErrorState>(object, [&] { return value; });
        if (created != value) {
            b->release(value->domain); b->release(value->user_info);
            fail("NSError initialized twice");
        }
    }
    Address create(Address domain, std::int64_t code, Address user_info) {
        const auto object = b->objc.allocate(b->error_class);
        try { initialize(object, domain, code, user_info); }
        catch (...) { b->process.objects().release(object); throw; }
        return object;
    }
    Address info_value(const ErrorState& value, std::string_view key) {
        if (!value.user_info) return 0;
        const auto name = b->string(key);
        const auto result = b->lookup(value.user_info, name);
        b->release(name);
        return result;
    }
    std::string description_text(Address object) {
        const auto value = state(object);
        if (const auto text = b->maybe_text(info_value(*value, description_key))) return *text;
        if (const auto reason = b->maybe_text(info_value(*value, "NSLocalizedFailureReason")))
            return "The operation couldn\xE2\x80\x99t be completed. " + *reason;
        return "The operation couldn\xE2\x80\x99t be completed. (" + b->text(value->domain) + " error " +
            std::to_string(value->code) + ".)";
    }
};

} // namespace

Address Bridge::error(std::string_view domain, std::int64_t code, std::string_view description,
                      const std::vector<std::pair<std::string, Address>>& user_info) {
    std::vector<std::pair<Address, Address>> entries;
    std::vector<Address> owned;
    const auto cleanup = [&] { release_all(owned); };
    try {
        if (!description.empty()) {
            const auto key = string(description_key), value = string(description);
            owned.push_back(key); owned.push_back(value);
            entries.emplace_back(key, value);
        }
        for (const auto& [name, value] : user_info) {
            if (!value) continue;
            const auto key = string(name);
            owned.push_back(key);
            entries.emplace_back(key, value);
        }
        const auto info = entries.empty() ? 0 : dictionary(entries);
        if (info) owned.push_back(info);
        const auto domain_object = string(domain);
        owned.push_back(domain_object);
        const std::array<Address, 3> arguments{domain_object, static_cast<Address>(code), info};
        const auto result = call_address(symbol(core_foundation_library, "_CFErrorCreate"),
                                         std::array<Address, 4>{0, arguments[0], arguments[1], arguments[2]});
        cleanup();
        return result;
    } catch (...) { cleanup(); throw; }
}

Address Bridge::os_status_error(std::int32_t status, std::string_view description) {
    return error("NSOSStatusErrorDomain", status, description);
}

void install_errors(const std::shared_ptr<Bridge>& b) {
    auto errors = std::make_shared<Errors>(Errors{b, {}});
    const std::string foundation(foundation_library);
    const std::string core(core_foundation_library);

    // Domains and userInfo keys (Foundation names and their CF aliases share
    // one constant object each).
    const std::pair<const char*, const char*> foundation_constants[] = {
        {"_NSCocoaErrorDomain", "NSCocoaErrorDomain"}, {"_NSPOSIXErrorDomain", "NSPOSIXErrorDomain"},
        {"_NSOSStatusErrorDomain", "NSOSStatusErrorDomain"}, {"_NSMachErrorDomain", "NSMachErrorDomain"},
        {"_NSLocalizedDescriptionKey", "NSLocalizedDescription"}, {"_NSLocalizedFailureReasonErrorKey", "NSLocalizedFailureReason"},
        {"_NSLocalizedRecoverySuggestionErrorKey", "NSLocalizedRecoverySuggestion"},
        {"_NSLocalizedRecoveryOptionsErrorKey", "NSLocalizedRecoveryOptions"}, {"_NSRecoveryAttempterErrorKey", "NSRecoveryAttempter"},
        {"_NSHelpAnchorErrorKey", "NSHelpAnchor"}, {"_NSDebugDescriptionErrorKey", "NSDebugDescription"},
        {"_NSLocalizedFailureErrorKey", "NSLocalizedFailure"}, {"_NSUnderlyingErrorKey", "NSUnderlyingError"},
        {"_NSMultipleUnderlyingErrorsKey", "NSMultipleUnderlyingErrorsKey"}, {"_NSStringEncodingErrorKey", "NSStringEncodingErrorKey"},
        {"_NSURLErrorKey", "NSURL"}, {"_NSFilePathErrorKey", "NSFilePath"}};
    for (const auto& [symbol, value] : foundation_constants) errors->export_constant(foundation, symbol, value);
    const std::pair<const char*, const char*> cf_constants[] = {
        {"_kCFErrorDomainPOSIX", "NSPOSIXErrorDomain"}, {"_kCFErrorDomainOSStatus", "NSOSStatusErrorDomain"},
        {"_kCFErrorDomainMach", "NSMachErrorDomain"}, {"_kCFErrorDomainCocoa", "NSCocoaErrorDomain"},
        {"_kCFErrorLocalizedDescriptionKey", "NSLocalizedDescription"}, {"_kCFErrorLocalizedFailureKey", "NSLocalizedFailure"},
        {"_kCFErrorLocalizedFailureReasonKey", "NSLocalizedFailureReason"},
        {"_kCFErrorLocalizedRecoverySuggestionKey", "NSLocalizedRecoverySuggestion"}, {"_kCFErrorDescriptionKey", "NSDescription"},
        {"_kCFErrorUnderlyingErrorKey", "NSUnderlyingError"}, {"_kCFErrorURLKey", "NSURL"}, {"_kCFErrorFilePathKey", "NSFilePath"}};
    for (const auto& [symbol, value] : cf_constants) errors->export_constant(core, symbol, value);

    const bool existing = b->objc.find_class("NSError") != 0;
    b->error_class = b->define_class("NSError", b->object_class, foundation_library);
    b->register_type_id(b->error_class, TypeId::error);
    if (existing && b->exported(core, "_CFErrorCreate")) return;   // another provider owns NSError
    const auto cls = b->error_class;

    b->method(cls, "errorWithDomain:code:userInfo:", [errors](CpuState& s, GuestMemory&) {
        s.x[0] = errors->b->autorelease(errors->create(s.x[2], static_cast<std::int64_t>(s.x[3]), s.x[4]));
    }, true, "@@:@q@");
    b->method(cls, "initWithDomain:code:userInfo:", [errors](CpuState& s, GuestMemory&) {
        try { errors->initialize(s.x[0], s.x[2], static_cast<std::int64_t>(s.x[3]), s.x[4]); }
        catch (...) { errors->b->release(s.x[0]); throw; }
    }, false, "@@:@q@");
    b->method(cls, "dealloc", [errors, cls](CpuState& s, GuestMemory&) {
        if (const auto value = errors->b->objc.find_host_state<ErrorState>(s.x[0])) {
            const auto domain = std::exchange(value->domain, 0), info = std::exchange(value->user_info, 0);
            errors->b->release(domain);
            errors->b->release(info);
        }
        static_cast<void>(errors->b->objc.send_super(s.x[0], cls, errors->b->objc.selector("dealloc")));
    }, false, "v@:");
    b->method(cls, "domain", [errors](CpuState& s, GuestMemory&) { s.x[0] = errors->state(s.x[0])->domain; });
    b->method(cls, "code", [errors](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(errors->state(s.x[0])->code); }, false, "q@:");
    b->method(cls, "userInfo", [errors](CpuState& s, GuestMemory&) {
        const auto value = errors->state(s.x[0]);
        s.x[0] = value->user_info ? value->user_info : errors->b->autorelease(errors->b->dictionary({}));
    });
    b->method(cls, "localizedDescription", [errors](CpuState& s, GuestMemory&) {
        s.x[0] = errors->b->autorelease(errors->b->string(errors->description_text(s.x[0])));
    });
    const std::pair<const char*, const char*> optional_text[] = {
        {"localizedFailureReason", "NSLocalizedFailureReason"}, {"localizedRecoverySuggestion", "NSLocalizedRecoverySuggestion"},
        {"localizedRecoveryOptions", "NSLocalizedRecoveryOptions"}, {"recoveryAttempter", "NSRecoveryAttempter"},
        {"helpAnchor", "NSHelpAnchor"}};
    for (const auto& [selector, key] : optional_text) {
        const std::string name(key);
        b->method(cls, selector, [errors, name](CpuState& s, GuestMemory&) { s.x[0] = errors->info_value(*errors->state(s.x[0]), name); });
    }
    b->method(cls, "underlyingErrors", [errors](CpuState& s, GuestMemory&) {
        const auto underlying = errors->info_value(*errors->state(s.x[0]), "NSUnderlyingError");
        std::vector<Address> values;
        if (underlying) values.push_back(underlying);
        s.x[0] = errors->b->autorelease(errors->b->array(values));
    });
    const auto describe = [errors](CpuState& s, GuestMemory&) {
        const auto value = errors->state(s.x[0]);
        const auto text = "Error Domain=" + errors->b->text(value->domain) + " Code=" + std::to_string(value->code) +
            " \"" + errors->description_text(s.x[0]) + "\"";
        s.x[0] = errors->b->autorelease(errors->b->string(text));
    };
    b->method(cls, "description", describe);
    b->method(cls, "debugDescription", describe);
    b->method(cls, "isEqual:", [errors](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[2]) { s.x[0] = 1; return; }
        const auto left = errors->b->objc.find_host_state<ErrorState>(s.x[0]);
        const auto right = s.x[2] ? errors->b->objc.find_host_state<ErrorState>(s.x[2]) : nullptr;
        s.x[0] = left && right && left->code == right->code && errors->b->cf.equal(left->domain, right->domain) &&
            (left->user_info == right->user_info ||
             (left->user_info && right->user_info && errors->b->cf.equal(left->user_info, right->user_info)));
    }, false, "B@:@");
    b->method(cls, "hash", [errors](CpuState& s, GuestMemory&) {
        const auto value = errors->state(s.x[0]);
        s.x[0] = errors->b->cf.hash(value->domain) ^ static_cast<Address>(value->code);
    }, false, "Q@:");
    b->method(cls, "copyWithZone:", [errors](CpuState& s, GuestMemory&) { s.x[0] = errors->b->retain(s.x[0]); }, false, "@@:^v");

    // ---- CFError (toll-free bridged to the class above) ----------------------------------
    if (b->exported(core, "_CFErrorCreate")) return;
    b->function(core, "_CFErrorCreate", [errors](CpuState& s, GuestMemory&) {
        s.x[0] = errors->create(s.x[1], static_cast<std::int64_t>(s.x[2]), s.x[3]);
    });
    b->function(core, "_CFErrorCreateWithUserInfoKeysAndValues", [errors](CpuState& s, GuestMemory& m) {
        if (s.x[5] > 4096) fail("CFErrorCreateWithUserInfoKeysAndValues: too many entries");
        std::vector<std::pair<Address, Address>> entries;
        for (Address index = 0; index < s.x[5]; ++index)
            entries.emplace_back(m.read64(s.x[3] + index * 8), m.read64(s.x[4] + index * 8));
        const auto info = entries.empty() ? 0 : errors->b->dictionary(entries);
        try { s.x[0] = errors->create(s.x[1], static_cast<std::int64_t>(s.x[2]), info); }
        catch (...) { errors->b->release(info); throw; }
        errors->b->release(info);
    });
    b->function(core, "_CFErrorGetDomain", [errors](CpuState& s, GuestMemory&) { s.x[0] = errors->state(s.x[0])->domain; });
    b->function(core, "_CFErrorGetCode", [errors](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(errors->state(s.x[0])->code); });
    b->function(core, "_CFErrorCopyUserInfo", [errors](CpuState& s, GuestMemory&) {
        const auto value = errors->state(s.x[0]);
        s.x[0] = value->user_info ? errors->b->retain(value->user_info) : errors->b->dictionary({});
    });
    b->function(core, "_CFErrorCopyDescription", [errors](CpuState& s, GuestMemory&) {
        s.x[0] = errors->b->string(errors->description_text(s.x[0]));
    });
    for (const auto& [symbol, key] : {std::pair{"_CFErrorCopyFailureReason", "NSLocalizedFailureReason"},
                                      std::pair{"_CFErrorCopyRecoverySuggestion", "NSLocalizedRecoverySuggestion"}}) {
        const std::string name(key);
        b->function(core, symbol, [errors, name](CpuState& s, GuestMemory&) {
            s.x[0] = errors->b->retain(errors->info_value(*errors->state(s.x[0]), name));
        });
    }
    b->function(core, "_CFErrorGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::error); });
}

} // namespace alchemy::gui::security
