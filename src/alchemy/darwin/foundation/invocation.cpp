#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/foundation/invocation_core.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

// NSMethodSignature and NSInvocation (Foundation), and the NSObject
// signature queries (-methodSignatureForSelector:,
// +instanceMethodSignatureForSelector:).
//
// A signature parses its Objective-C type encoding (invocation_core.cpp) and
// keeps guest copies of its type strings for -getArgumentTypeAtIndex: and
// -methodReturnType. An invocation stores each argument's bytes and the
// return value's bytes; -invoke sends the message with the arguments placed
// as the Apple arm64 calling convention places them (x/v registers, packed
// stack, composites over 16 bytes by reference, large results through x8).
// -retainArguments retains object arguments, copies blocks and C strings, and
// keeps them until the invocation is deallocated or they are replaced.
// Unsupported type encodings (bitfields, vectors) fail explicitly.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;
namespace inv = invocation;

struct SignatureState final : ObjectPayload {
    ProcessContext* process{};
    inv::Signature signature;
    inv::Layout layout;
    std::vector<Address> strings;   // guest: full types, return type, argument types
    ~SignatureState() override {
        for (const auto address : strings) if (address) process->release_guest(address);
    }
};

struct InvocationState final : ObjectPayload {
    std::mutex mutex;
    Address signature{};                         // +1 NSMethodSignature
    std::shared_ptr<SignatureState> layout;
    std::vector<std::vector<std::uint8_t>> arguments;
    std::vector<std::uint8_t> result;
    bool retained{};
    std::vector<std::pair<Address, bool>> owned; // retained objects / copied blocks (true) (+1)
    std::vector<Address> strings;                // guest C string copies
};

Address guest_string(Support& support, std::string_view text) {
    const auto address = support.process.allocate_guest(text.size() + 1, 1);
    if (!text.empty()) support.memory().write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    support.memory().write8(address + text.size(), 0);
    return address;
}

std::uint64_t load(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t size) {
    std::uint64_t value{};
    if (offset < bytes.size()) std::memcpy(&value, bytes.data() + offset, std::min(size, bytes.size() - offset));
    return value;
}

std::uint64_t extend(const inv::Type& type, std::uint64_t value) {
    if (type.size >= 8 || type.kind != inv::Kind::integer) return value;
    const auto bits = type.size * 8;
    value &= (std::uint64_t{1} << bits) - 1;
    if (type.is_signed && (value >> (bits - 1)) & 1) value |= ~((std::uint64_t{1} << bits) - 1);
    return value;
}

} // namespace

// The NSMethodSignature state of a signature object (shared by the providers
// of this directory: undo proxies capture invocations through it).
std::shared_ptr<ObjectPayload> method_signature_state(Support& support, Address signature);

namespace {

struct Invocations {
    Support& support;
    Address signature_class{}, invocation_class{};

    std::shared_ptr<SignatureState> signature_of(Address object) {
        auto state = support.objc.find_host_state<SignatureState>(object);
        if (!state) fail("not an initialized NSMethodSignature");
        return state;
    }
    std::shared_ptr<InvocationState> invocation_of(Address object) {
        auto state = support.objc.find_host_state<InvocationState>(object);
        if (!state || !state->layout) fail("not an initialized NSInvocation");
        return state;
    }
    // +1 signature object for a type encoding.
    Address make_signature(std::string_view types) {
        auto state = std::make_shared<SignatureState>();
        state->process = &support.process;
        state->signature = inv::parse_signature(types);
        state->layout = inv::assign(state->signature);
        state->strings.push_back(guest_string(support, types));
        state->strings.push_back(guest_string(support, state->signature.result.encoding));
        for (const auto& argument : state->signature.arguments) state->strings.push_back(guest_string(support, argument.encoding));
        return make_object(support, signature_class, state);
    }
    // The signature of `selector` for instances of `cls`, or nil (+0).
    Address signature_for(Address cls, Address selector) {
        if (!cls || !selector) return 0;
        const auto method = support.call(libobjc, "_class_getInstanceMethod", {cls, selector});
        if (!method) return 0;
        const auto types = support.call(libobjc, "_method_getTypeEncoding", {method});
        if (!types) return 0;
        return autoreleased(support, make_signature(c_string(support.memory(), types, 4096)));
    }

    // Keeps an argument alive when the invocation retains its arguments.
    void own(InvocationState& state, const inv::Type& type, std::vector<std::uint8_t>& bytes) {
        if (!state.retained) return;
        const auto value = load(bytes, 0, 8);
        if (!value) return;
        if (type.block) state.owned.emplace_back(block_copy(support, value), true);
        else if (type.object) state.owned.emplace_back(objc_retain(support, value), false);
        else if (type.c_string) {
            const auto copy = guest_string(support, c_string(support.memory(), value));
            state.strings.push_back(copy);
            std::memcpy(bytes.data(), &copy, 8);
        }
    }
    void retain_all(InvocationState& state, bool keep_target = true) {
        if (state.retained) return;
        state.retained = true;
        const auto& types = state.layout->signature.arguments;
        for (std::size_t index = 0; index < types.size(); ++index) {
            if (index == 1 || (index == 0 && !keep_target)) continue;   // _cmd; the target when not kept
            own(state, types[index], state.arguments[index]);
        }
    }

    Address make_invocation(Address signature) {
        const auto layout = signature_of(signature);
        auto state = std::make_shared<InvocationState>();
        state->signature = support.retain(signature);
        state->layout = layout;
        for (const auto& type : layout->signature.arguments) state->arguments.emplace_back(type.size, 0);
        state->result.assign(layout->signature.result.size, 0);
        return make_object(support, invocation_class, state);
    }

    void invoke(Address invocation, std::optional<Address> target_override) {
        const auto state = invocation_of(invocation);
        std::unique_lock lock(state->mutex);
        if (target_override) std::memcpy(state->arguments[0].data(), &*target_override, 8);
        const auto target = load(state->arguments[0], 0, 8), selector = load(state->arguments[1], 0, 8);
        const auto& signature = state->layout->signature;
        const auto& layout = state->layout->layout;
        std::fill(state->result.begin(), state->result.end(), 0);
        if (!target) return;   // messages to nil return zero
        if (!selector) fail("-[NSInvocation invoke] without a selector");
        CallRegs registers;
        std::vector<std::unique_ptr<GuestBlock>> copies;
        registers.stack.assign(layout.stack_size, 0);
        for (std::size_t index = 2; index < signature.arguments.size(); ++index) {
            const auto& type = signature.arguments[index];
            const auto& bytes = state->arguments[index];
            const auto& at = layout.arguments[index];
            switch (at.where) {
            case inv::Location::Where::x:
                for (unsigned word = 0; word < at.count; ++word)
                    registers.x[at.first + word] = word == 0 && at.count == 1 ? extend(type, load(bytes, 0, type.size)) : load(bytes, word * 8, 8);
                break;
            case inv::Location::Where::v:
                if (type.kind == inv::Kind::floating) registers.v[at.first] = {load(bytes, 0, type.size), 0};
                else for (unsigned member = 0; member < at.count; ++member)
                    registers.v[at.first + member] = {load(bytes, member * type.hfa_member, type.hfa_member), 0};
                break;
            case inv::Location::Where::stack:
                std::memcpy(registers.stack.data() + at.offset, bytes.data(), bytes.size());
                break;
            case inv::Location::Where::reference: {
                copies.push_back(std::make_unique<GuestBlock>(support, type.size, false));
                support.memory().write_bytes(*copies.back(), bytes);
                const Address pointer = *copies.back();
                if (at.stack_pointer) std::memcpy(registers.stack.data() + at.offset, &pointer, 8);
                else registers.x[at.first] = pointer;
                break;
            }
            }
        }
        std::unique_ptr<GuestBlock> indirect;
        if (layout.indirect_result) {
            indirect = std::make_unique<GuestBlock>(support, signature.result.size);
            registers.indirect_result = *indirect;
        }
        lock.unlock();   // the callee may use the invocation
        const auto result = support.objc.send(target, selector, registers);
        lock.lock();
        const auto& type = signature.result;
        if (type.kind == inv::Kind::void_type) return;
        auto& out = state->result;
        const auto put = [&](std::size_t offset, std::uint64_t value, std::size_t size) {
            if (offset < out.size()) std::memcpy(out.data() + offset, &value, std::min(size, out.size() - offset));
        };
        if (layout.indirect_result) { support.memory().read_bytes(*indirect, out); return; }
        if (type.kind == inv::Kind::integer) { put(0, result.x0, type.size); return; }
        if (!result.complete) unsupported("-[NSInvocation invoke] of a method returning " + type.encoding + " on an engine without full result registers");
        if (type.kind == inv::Kind::floating) { put(0, result.v[0][0], type.size); return; }
        if (type.hfa_count) {
            for (unsigned member = 0; member < type.hfa_count && member < 4; ++member) put(member * type.hfa_member, result.v[member][0], type.hfa_member);
            return;
        }
        put(0, result.x0, 8);
        put(8, result.x1, 8);
    }

    // Arguments from a call's register image (an undo or forwarding proxy
    // receiving the message): a new invocation (+1) that retains them.
    Address capture(Address signature, Address target, Address selector, const CpuState& s, bool keep_target) {
        const auto object = make_invocation(signature);
        const auto state = invocation_of(object);
        std::lock_guard lock(state->mutex);
        std::memcpy(state->arguments[0].data(), &target, 8);
        std::memcpy(state->arguments[1].data(), &selector, 8);
        const auto& signature_value = state->layout->signature;
        const auto& layout = state->layout->layout;
        auto& memory = support.memory();
        for (std::size_t index = 2; index < signature_value.arguments.size(); ++index) {
            const auto& type = signature_value.arguments[index];
            auto& bytes = state->arguments[index];
            const auto& at = layout.arguments[index];
            const auto store_word = [&](std::size_t offset, std::uint64_t value, std::size_t size) {
                if (offset < bytes.size()) std::memcpy(bytes.data() + offset, &value, std::min(size, bytes.size() - offset));
            };
            switch (at.where) {
            case inv::Location::Where::x:
                for (unsigned word = 0; word < at.count; ++word) store_word(word * 8, s.x[at.first + word], 8);
                break;
            case inv::Location::Where::v:
                if (type.kind == inv::Kind::floating) store_word(0, s.v[at.first][0], type.size);
                else for (unsigned member = 0; member < at.count; ++member) store_word(member * type.hfa_member, s.v[at.first + member][0], type.hfa_member);
                break;
            case inv::Location::Where::stack:
                memory.read_bytes(s.sp + at.offset, bytes);
                break;
            case inv::Location::Where::reference: {
                const auto pointer = at.stack_pointer ? memory.read64(s.sp + at.offset) : s.x[at.first];
                memory.read_bytes(pointer, bytes);
                break;
            }
            }
        }
        retain_all(*state, keep_target);
        return object;
    }
};

std::shared_ptr<Invocations> invocations;

} // namespace

std::shared_ptr<ObjectPayload> method_signature_state(Support& support, Address signature) {
    return support.objc.find_host_state<SignatureState>(signature);
}

// +1 NSInvocation for a message received by a proxy (see capture above).
Address capture_invocation(Support& support, Address signature, Address target, Address selector, const CpuState& state, bool keep_target) {
    static_cast<void>(support);
    if (!invocations) fail("NSInvocation is not installed");
    return invocations->capture(signature, target, selector, state, keep_target);
}

// The signature (+0) of an instance method of `cls`, or nil.
Address method_signature(Support& support, Address cls, Address selector) {
    static_cast<void>(support);
    if (!invocations) fail("NSInvocation is not installed");
    return invocations->signature_for(cls, selector);
}

void install_invocations(Support& support) {
    auto self = std::make_shared<Invocations>(Invocations{support});
    invocations = self;
    self->signature_class = support.declare_class("NSMethodSignature", "NSObject", foundation_library);
    self->invocation_class = support.declare_class("NSInvocation", "NSObject", foundation_library);
    const auto sig = self->signature_class, cls = self->invocation_class;

    // ---- NSMethodSignature ----
    support.method(sig, "signatureWithObjCTypes:", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) fail("+[NSMethodSignature signatureWithObjCTypes:] with NULL types");
        s.x[0] = autoreleased(self->support, self->make_signature(c_string(m, s.x[2], 4096)));
    }, "@@:r*", true);
    support.method(sig, "numberOfArguments", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->signature_of(s.x[0])->signature.arguments.size();
    }, "Q@:");
    support.method(sig, "getArgumentTypeAtIndex:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->signature_of(s.x[0]);
        if (s.x[2] >= state->signature.arguments.size())
            fail("NSInvalidArgumentException: -[NSMethodSignature getArgumentTypeAtIndex:] index " + std::to_string(s.x[2]) + " out of bounds");
        s.x[0] = state->strings[2 + s.x[2]];
    }, "r*@:Q");
    support.method(sig, "methodReturnType", [self](CpuState& s, GuestMemory&) { s.x[0] = self->signature_of(s.x[0])->strings[1]; }, "r*@:");
    support.method(sig, "methodReturnLength", [self](CpuState& s, GuestMemory&) { s.x[0] = self->signature_of(s.x[0])->signature.result.size; }, "Q@:");
    support.method(sig, "frameLength", [self](CpuState& s, GuestMemory&) { s.x[0] = self->signature_of(s.x[0])->signature.frame_length; }, "Q@:");
    support.method(sig, "isOneway", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->signature_of(s.x[0])->signature.types.starts_with("V");
    }, "B@:");
    support.method(sig, "_typeString", [self](CpuState& s, GuestMemory&) {
        s.x[0] = string(self->support, self->signature_of(s.x[0])->signature.types);
    }, "@@:");
    support.method(sig, "isEqual:", [self](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? self->support.objc.find_host_state<SignatureState>(s.x[2]) : nullptr;
        s.x[0] = other && other->signature.types == self->signature_of(s.x[0])->signature.types;
    }, "B@:@");
    support.method(sig, "hash", [self](CpuState& s, GuestMemory&) {
        s.x[0] = std::hash<std::string>{}(self->signature_of(s.x[0])->signature.types);
    }, "Q@:");
    support.method(sig, "copyWithZone:", [self](CpuState& s, GuestMemory&) { s.x[0] = self->support.retain(s.x[0]); }, "@@:^v");
    support.method(sig, "debugDescription", [self](CpuState& s, GuestMemory&) {
        s.x[0] = string(self->support, "<NSMethodSignature: types=" + self->signature_of(s.x[0])->signature.types + ">");
    }, "@@:");

    // ---- signature queries on NSObject ----
    const auto root = class_named(support, "NSObject");
    support.method(root, "methodSignatureForSelector:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->signature_for(self->support.objc.object_class(s.x[0]), s.x[2]);
    }, "@@::");
    // Class receivers: their class methods (the metaclass's instance methods).
    support.method(root, "methodSignatureForSelector:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->signature_for(self->support.objc.object_class(s.x[0]), s.x[2]);
    }, "@@::", true);
    support.method(root, "instanceMethodSignatureForSelector:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->signature_for(s.x[0], s.x[2]);
    }, "@@::", true);

    // ---- NSInvocation ----
    support.method(cls, "invocationWithMethodSignature:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("NSInvalidArgumentException: +[NSInvocation invocationWithMethodSignature:] with a nil signature");
        s.x[0] = autoreleased(self->support, self->make_invocation(s.x[2]));
    }, "@@:@", true);
    support.method(cls, "methodSignature", [self](CpuState& s, GuestMemory&) { s.x[0] = self->invocation_of(s.x[0])->signature; }, "@@:");
    const auto argument_index = [self](Address object, std::int64_t index, bool getting) {
        const auto state = self->invocation_of(object);
        if (index < 0 || static_cast<std::size_t>(index) >= state->arguments.size())
            fail("NSInvalidArgumentException: -[NSInvocation " + std::string(getting ? "getArgument" : "setArgument") +
                 ":atIndex:] index " + std::to_string(index) + " out of bounds");
        return state;
    };
    support.method(cls, "setArgument:atIndex:", [self, argument_index](CpuState& s, GuestMemory& m) {
        const auto index = static_cast<std::int64_t>(s.x[3]);
        const auto state = argument_index(s.x[0], index, false);
        std::lock_guard lock(state->mutex);
        auto& bytes = state->arguments[static_cast<std::size_t>(index)];
        if (!s.x[2]) fail("-[NSInvocation setArgument:atIndex:] with a NULL buffer");
        m.read_bytes(s.x[2], bytes);
        if (index != 1) self->own(*state, state->layout->signature.arguments[static_cast<std::size_t>(index)], bytes);
    }, "v@:^vq");
    support.method(cls, "getArgument:atIndex:", [argument_index](CpuState& s, GuestMemory& m) {
        const auto index = static_cast<std::int64_t>(s.x[3]);
        const auto state = argument_index(s.x[0], index, true);
        std::lock_guard lock(state->mutex);
        if (!s.x[2]) fail("-[NSInvocation getArgument:atIndex:] with a NULL buffer");
        m.write_bytes(s.x[2], state->arguments[static_cast<std::size_t>(index)]);
    }, "v@:^vq");
    const auto word_accessor = [self](std::size_t index) {
        return [self, index](CpuState& s, GuestMemory&) {
            const auto state = self->invocation_of(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = load(state->arguments[index], 0, 8);
        };
    };
    support.method(cls, "target", word_accessor(0), "@@:");
    support.method(cls, "selector", word_accessor(1), ":@:");
    support.method(cls, "setTarget:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->invocation_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        std::memcpy(state->arguments[0].data(), &s.x[2], 8);
        self->own(*state, state->layout->signature.arguments[0], state->arguments[0]);
    }, "v@:@");
    support.method(cls, "setSelector:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->invocation_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        std::memcpy(state->arguments[1].data(), &s.x[2], 8);
    }, "v@::");
    support.method(cls, "retainArguments", [self](CpuState& s, GuestMemory&) {
        const auto state = self->invocation_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        self->retain_all(*state);
    }, "v@:");
    support.method(cls, "argumentsRetained", [self](CpuState& s, GuestMemory&) {
        const auto state = self->invocation_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->retained;
    }, "B@:");
    support.method(cls, "invoke", [self](CpuState& s, GuestMemory&) { self->invoke(s.x[0], std::nullopt); }, "v@:");
    support.method(cls, "invokeWithTarget:", [self](CpuState& s, GuestMemory&) { self->invoke(s.x[0], s.x[2]); }, "v@:@");
    support.method(cls, "getReturnValue:", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->invocation_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        if (!state->result.empty()) {
            if (!s.x[2]) fail("-[NSInvocation getReturnValue:] with a NULL buffer");
            m.write_bytes(s.x[2], state->result);
        }
    }, "v@:^v");
    support.method(cls, "setReturnValue:", [self](CpuState& s, GuestMemory& m) {
        const auto state = self->invocation_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        if (!state->result.empty()) {
            if (!s.x[2]) fail("-[NSInvocation setReturnValue:] with a NULL buffer");
            m.read_bytes(s.x[2], state->result);
        }
    }, "v@:^v");
    support.method(cls, "dealloc", [self, cls](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<InvocationState>(s.x[0])) {
            std::vector<std::pair<Address, bool>> owned;
            std::vector<Address> strings;
            Address signature{};
            {
                std::lock_guard lock(state->mutex);
                owned.swap(state->owned);
                strings.swap(state->strings);
                signature = std::exchange(state->signature, 0);
            }
            for (const auto& [value, block] : owned) {
                if (block) block_release(self->support, value);
                else objc_release(self->support, value);
            }
            for (const auto value : strings) self->support.process.release_guest(value);
            self->support.release(signature);
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], cls, self->support.objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit
