#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include "alchemy/engine_adapter.hpp"

namespace alchemy::gui::completion_kit::rt {

std::string c_string(GuestMemory& memory, Address address, std::size_t limit) {
    std::string text;
    if (!address) return text;
    for (std::size_t index = 0; index < limit; ++index) {
        const auto c = memory.read8(address + index);
        if (!c) return text;
        text.push_back(static_cast<char>(c));
    }
    fail("guest C string exceeds " + std::to_string(limit) + " bytes");
}

GuestBlock::GuestBlock(Support& support, std::size_t size, bool zero) : support_(support) {
    address_ = support.process.allocate_guest(size ? size : 1, 16);
    if (zero) support.memory().fill_bytes(address_, 0, size ? size : 1);
}

GuestBlock::GuestBlock(Support& support, std::string_view value) : support_(support) {
    address_ = support.process.allocate_guest(value.size() + 1, 16);
    if (!value.empty())
        support.memory().write_bytes(address_, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    support.memory().write8(address_ + value.size(), 0);
}

GuestBlock::~GuestBlock() { if (address_) support_.process.release_guest(address_); }

std::string text(Support& support, Address string) { return string ? support.cf.utf8(string) : std::string{}; }

Address autoreleased(Support& support, Address object) { return object ? support.send_id(object, "autorelease") : 0; }

Address string(Support& support, std::string_view utf8) { return autoreleased(support, support.cf.string(utf8)); }

Address class_named(Support& support, std::string_view name) {
    const auto cls = support.objc.find_class(name);
    if (!cls) fail(std::string(name) + " is not provided");
    return cls;
}

std::string selector_name(Support& support, Address selector) {
    if (!selector) return {};
    return c_string(support.memory(), support.call(libobjc, "_sel_getName", {selector}), 4096);
}

Address number(Support& support, std::int64_t value) { return autoreleased(support, support.cf.integer(value, 4)); }

Address boolean(Support& support, bool value) {
    return support.send_id(class_named(support, "NSNumber"), "numberWithBool:", {value ? Address{1} : Address{0}});
}

Address dictionary(Support& support, std::initializer_list<std::pair<std::string_view, Address>> entries) {
    std::vector<std::pair<Address, Address>> pairs;
    std::vector<Address> keys;
    for (const auto& [name, value] : entries) {
        if (!value) continue;
        keys.push_back(support.cf.string(name));
        pairs.emplace_back(keys.back(), value);
    }
    const auto result = support.cf.dictionary(pairs);
    for (const auto key : keys) support.release(key);
    return autoreleased(support, result);
}

Address array(Support& support, std::span<const Address> values) { return autoreleased(support, support.cf.array(values)); }

Address error(Support& support, std::string_view domain, std::int64_t code, std::string_view description,
              std::initializer_list<std::pair<std::string_view, Address>> info) {
    std::vector<std::pair<Address, Address>> pairs;
    std::vector<Address> owned;
    if (!description.empty()) {
        owned.push_back(support.cf.string("NSLocalizedDescription"));
        owned.push_back(support.cf.string(description));
        pairs.emplace_back(owned[owned.size() - 2], owned.back());
    }
    for (const auto& [name, value] : info) {
        if (!value) continue;
        owned.push_back(support.cf.string(name));
        pairs.emplace_back(owned.back(), value);
    }
    const auto user_info = pairs.empty() ? 0 : support.cf.dictionary(pairs);
    for (const auto value : owned) support.release(value);
    const auto domain_string = support.cf.string(domain);
    CallRegs registers;
    registers.x[2] = domain_string;
    registers.x[3] = static_cast<Address>(code);
    registers.x[4] = user_info;
    const auto result = support.send(class_named(support, "NSError"), "errorWithDomain:code:userInfo:", registers).x0;
    support.release(domain_string);
    support.release(user_info);
    return result;
}

void store(Support& support, Address out, Address value) { if (out) support.memory().write64(out, value); }

std::uint32_t guest_errno(Support& support) {
    const auto location = support.call(libsystem, "___error", {});
    return location ? support.memory().read32(location) : 0;
}

Address objc_retain(Support& support, Address object) { return object ? support.call(libobjc, "_objc_retain", {object}) : 0; }

void objc_release(Support& support, Address object) { if (object) static_cast<void>(support.call(libobjc, "_objc_release", {object})); }

Address block_copy(Support& support, Address block) { return block ? support.call(libsystem, "__Block_copy", {block}) : 0; }

void block_release(Support& support, Address block) { if (block) static_cast<void>(support.call(libsystem, "__Block_release", {block})); }

CallResult invoke_block(Support& support, Address block, CallRegs registers) {
    if (!block) fail("a nil block was invoked");
    registers.x[0] = block;
    return EngineAdapter(support.process.calling_engine()).call_regs(support.memory().read64(block + 16), registers);
}

Address invoke_block(Support& support, Address block, std::initializer_list<Address> arguments) {
    CallRegs registers;
    unsigned index = 1;
    for (const auto value : arguments) registers.x.at(index++) = value;
    return invoke_block(support, block, registers).x0;
}

void post_notification(Support& support, std::string_view name, Address object, Address user_info) {
    const auto center = support.send_id(class_named(support, "NSNotificationCenter"), "defaultCenter");
    const auto name_string = support.cf.string(name);
    static_cast<void>(support.send_id(center, "postNotificationName:object:userInfo:", {name_string, object, user_info}));
    support.release(name_string);
}

Address call_address(Support& support, Address function, std::initializer_list<Address> arguments) {
    return EngineAdapter(support.process.calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
}

} // namespace alchemy::gui::completion_kit::rt
