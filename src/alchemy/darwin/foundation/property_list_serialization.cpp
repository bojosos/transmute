#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"

#include <mutex>
#include <string>
#include <vector>

// Foundation gaps of the Qt WebEngine images:
//
// Foundation: NSPropertyListSerialization (over CFPropertyList), the HFS
// type-code helpers, NSAssertionHandler (an assertion failure stops the
// process with the method, file and line, as an uncaught
// NSInternalInconsistencyException does: Objective-C exceptions are not
// supported).
namespace alchemy::gui::fill {
namespace {

class FoundationGaps final : public Provider {
public:
    using Provider::Provider;
    Address assertion_handler{};
    std::mutex mutex;

    std::string class_name(Address object) {
        if (!object) return "nil";
        try { return read_cstring(memory(), call(libobjc, "_object_getClassName", {object})); } catch (const std::exception&) { return "?"; }
    }
    std::string text(Address string) { return string ? cf.utf8(string) : std::string(); }

    void install() {
        string(foundation, "_NSArgumentDomain", "NSArgumentDomain");
        string(foundation, "_NSProcessInfoThermalStateDidChangeNotification", "NSProcessInfoThermalStateDidChangeNotification");

        // OSType NSHFSTypeCodeFromFileType(NSString *): "'abcd'" -> 'abcd', otherwise 0.
        raw(foundation, "_NSHFSTypeCodeFromFileType", [this](CpuState& s, GuestMemory&) {
            const auto value = text(s.x[0]);
            if (value.size() != 6 || value.front() != '\'' || value.back() != '\'') { s.x[0] = 0; return; }
            std::uint32_t code = 0;
            for (std::size_t i = 1; i < 5; ++i) code = (code << 8) | static_cast<unsigned char>(value[i]);
            s.x[0] = code;
        });
        // NSString *NSHFSTypeOfFile(NSString *path): files under Alchemy carry no HFS
        // type code, so an existing file answers "''" (type 0); a missing file nil.
        raw(foundation, "_NSHFSTypeOfFile", [this](CpuState& s, GuestMemory&) {
            if (!s.x[0]) { s.x[0] = 0; return; }
            const auto path = strdup(text(s.x[0]));
            const auto missing = static_cast<std::int32_t>(call(libsystem, "_access", {path, 0})) != 0;
            free(path);
            s.x[0] = missing ? 0 : bridge->autorelease(cf.string("''"));
        });

        // NSPropertyListSerialization over CFPropertyList.
        const auto plist = cls(foundation, "NSPropertyListSerialization", "NSObject");
        const auto autorelease_error = [this](GuestMemory& m, Address out) {
            if (out && m.read64(out)) m.write64(out, bridge->autorelease(m.read64(out)));
        };
        method(plist, "dataWithPropertyList:format:options:error:", [this, autorelease_error](CpuState& s, GuestMemory& m) {
            if (s.x[5]) m.write64(s.x[5], 0);
            const auto result = call(core_foundation, "_CFPropertyListCreateData", {0, s.x[2], s.x[3], s.x[4], s.x[5]});
            autorelease_error(m, s.x[5]);
            s.x[0] = bridge->autorelease(result);
        }, true, "@@:@QQ^@");
        method(plist, "propertyListWithData:options:format:error:", [this, autorelease_error](CpuState& s, GuestMemory& m) {
            if (s.x[5]) m.write64(s.x[5], 0);
            const auto result = call(core_foundation, "_CFPropertyListCreateWithData", {0, s.x[2], s.x[3], s.x[4], s.x[5]});
            autorelease_error(m, s.x[5]);
            s.x[0] = bridge->autorelease(result);
        }, true, "@@:@Q^Q^@");
        method(plist, "propertyList:isValidForFormat:", [this](CpuState& s, GuestMemory&) {
            s.x[0] = call(core_foundation, "_CFPropertyListIsValid", {s.x[2], s.x[3]}) & 0xff;
        }, true, "B@:@Q");
        const auto send = [this](Address receiver, std::string_view selector, std::initializer_list<Address> arguments) {
            CallRegs registers{};
            unsigned index = 2;
            for (const auto argument : arguments) registers.x[index++] = argument;
            return objc.send(receiver, selector, registers).x0;
        };
        // +writePropertyList:toStream:format:options:error: -> the bytes written (0 on error).
        method(plist, "writePropertyList:toStream:format:options:error:", [this, send, autorelease_error](CpuState& s, GuestMemory& m) {
            if (s.x[6]) m.write64(s.x[6], 0);
            const auto data = call(core_foundation, "_CFPropertyListCreateData", {0, s.x[2], s.x[4], s.x[5], s.x[6]});
            autorelease_error(m, s.x[6]);
            if (!data) { s.x[0] = 0; return; }
            const auto written = static_cast<std::int64_t>(send(s.x[3], "write:maxLength:",
                {call(core_foundation, "_CFDataGetBytePtr", {data}), call(core_foundation, "_CFDataGetLength", {data})}));
            release(data);
            s.x[0] = written < 0 ? 0 : static_cast<Address>(written);
        }, true, "q@:@@QQ^@");
        // +propertyListWithStream:options:format:error: reads the open stream to its end.
        method(plist, "propertyListWithStream:options:format:error:", [this, send, autorelease_error](CpuState& s, GuestMemory& m) {
            const auto data = call(core_foundation, "_CFDataCreateMutable", {0, 0});
            const auto buffer = malloc(65536);
            for (;;) {
                const auto count = static_cast<std::int64_t>(send(s.x[2], "read:maxLength:", {buffer, 65536}));
                if (count <= 0) break;
                static_cast<void>(call(core_foundation, "_CFDataAppendBytes", {data, buffer, static_cast<Address>(count)}));
            }
            free(buffer);
            if (s.x[5]) m.write64(s.x[5], 0);
            const auto result = call(core_foundation, "_CFPropertyListCreateWithData", {0, data, s.x[3], s.x[4], s.x[5]});
            release(data);
            autorelease_error(m, s.x[5]);
            s.x[0] = bridge->autorelease(result);
        }, true, "@@:@Q^Q^@");
        // The older forms report a failure as a string (the error's description).
        const auto description_of = [this, send](GuestMemory& m, Address error_slot, Address out) {
            if (!out) return;
            const auto error = m.read64(error_slot);
            m.write64(out, error ? send(error, "localizedDescription", {}) : 0);
        };
        method(plist, "dataFromPropertyList:format:errorDescription:", [this, description_of](CpuState& s, GuestMemory& m) {
            const auto error = malloc(8);
            m.write64(error, 0);
            const auto result = call(core_foundation, "_CFPropertyListCreateData", {0, s.x[2], s.x[3], 0, error});
            if (const auto failure = m.read64(error)) { m.write64(error, bridge->autorelease(failure)); }
            description_of(m, error, s.x[4]);
            free(error);
            s.x[0] = bridge->autorelease(result);
        }, true, "@@:@Q^@");
        method(plist, "propertyListFromData:mutabilityOption:format:errorDescription:", [this, description_of](CpuState& s, GuestMemory& m) {
            const auto error = malloc(8);
            m.write64(error, 0);
            const auto result = call(core_foundation, "_CFPropertyListCreateWithData", {0, s.x[2], s.x[3], s.x[4], error});
            if (const auto failure = m.read64(error)) { m.write64(error, bridge->autorelease(failure)); }
            description_of(m, error, s.x[5]);
            free(error);
            s.x[0] = bridge->autorelease(result);
        }, true, "@@:@Q^Q^@");

        // NSAssertionHandler: an assertion failure ends the process.
        const auto handler_class = cls(foundation, "NSAssertionHandler", "NSObject");
        method(handler_class, "currentHandler", [this, handler_class](CpuState& s, GuestMemory&) {
            std::lock_guard lock(mutex);
            if (!assertion_handler) {
                assertion_handler = objc.allocate(handler_class);
                if (const auto record = process.objects().find(assertion_handler)) record->immortal = true;
            }
            s.x[0] = assertion_handler;
        }, true);
        // -handleFailureInMethod:(SEL) object:(id) file:(NSString*) lineNumber:(NSInteger) description:(NSString*), ...
        method(handler_class, "handleFailureInMethod:object:file:lineNumber:description:", [this](CpuState& s, GuestMemory& m) {
            const auto selector = read_cstring(m, call(libobjc, "_sel_getName", {s.x[2]}));
            throw std::runtime_error("*** Assertion failure in -[" + class_name(s.x[3]) + " " + selector + "], " + text(s.x[4]) + ":" +
                std::to_string(static_cast<std::int64_t>(s.x[5])) + " (" + text(s.x[6]) + ")");
        }, false, "v@::@@q@");
        // -handleFailureInFunction:(NSString*) file:(NSString*) lineNumber:(NSInteger) description:(NSString*), ...
        method(handler_class, "handleFailureInFunction:file:lineNumber:description:", [this](CpuState& s, GuestMemory&) {
            throw std::runtime_error("*** Assertion failure in " + text(s.x[2]) + "(), " + text(s.x[3]) + ":" +
                std::to_string(static_cast<std::int64_t>(s.x[4])) + " (" + text(s.x[5]) + ")");
        }, false, "v@:@@q@");
    }
};

} // namespace

void install_foundation_gaps(InstallContext& context) { std::make_shared<FoundationGaps>(context)->install(); }

} // namespace alchemy::gui::fill
