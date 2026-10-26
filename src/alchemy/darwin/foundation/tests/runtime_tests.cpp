// Tests of Foundation's runtime providers (darwin/foundation/), split out of
// the QLab runtime group's tests (kit/completion/runtime/tests/runtime_tests.cpp):
// the host-only cores (type encodings and the arm64 argument layout, the undo
// stack, scripting definitions, bundle version numbers) and the providers over
// a reference-engine process: NSException, NSInvocation, NSUndoManager, KVO and
// Cocoa bindings, notification queues, operations, the file manager, Cocoa
// scripting, and CoreFoundation's class identities and CFFileDescriptor.
#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/foundation/invocation_core.hpp"
#include "alchemy/darwin/foundation/sdef.hpp"
#include "alchemy/darwin/foundation/undo_core.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"

#include <atomic>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit { void install_streams(Support& support); }   // streams.cpp

namespace {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
namespace completion_kit = alchemy::gui::completion_kit;
namespace inv = alchemy::gui::completion_kit::invocation;
namespace undo = alchemy::gui::completion_kit::undo;

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

template <class F>
std::string failure_of(F&& run) {
    try { run(); } catch (const std::exception& error) { return error.what(); }
    return {};
}

// ---- type encodings and the argument layout ----------------------------------------------------

void test_type_encodings() {
    const auto signature = inv::parse_signature("v40@0:8q16d24{CGPoint=dd}32");
    require(signature.arguments.size() == 5, "five arguments");
    require(signature.result.kind == inv::Kind::void_type, "void result");
    require(signature.arguments[2].size == 8 && signature.arguments[2].is_signed, "q is a signed 8-byte integer");
    require(signature.arguments[3].kind == inv::Kind::floating, "d is floating point");
    require(signature.arguments[4].hfa_count == 2 && signature.arguments[4].hfa_member == 8 && signature.arguments[4].size == 16,
            "CGPoint is a two-double HFA");
    const auto rect = inv::parse_signature("{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    require(rect.result.size == 32 && rect.result.hfa_count == 4, "CGRect is a four-double HFA of 32 bytes");
    const auto mixed = inv::parse_signature("v@:{S=cid}^{Opaque}@?r*@\"NSString\"[4f]");
    require(mixed.arguments[2].size == 16 && mixed.arguments[2].alignment == 8 && !mixed.arguments[2].hfa_count, "{S=cid} layout");
    require(mixed.arguments[3].size == 8 && mixed.arguments[3].kind == inv::Kind::integer, "pointer to an opaque struct");
    require(mixed.arguments[4].block && mixed.arguments[4].object, "@? is a block");
    require(mixed.arguments[5].c_string, "r* is a C string");
    require(mixed.arguments[6].object && mixed.arguments[6].encoding == "@", "quoted class names are skipped");
    require(mixed.arguments[7].size == 16 && mixed.arguments[7].hfa_count == 4 && mixed.arguments[7].hfa_member == 4, "[4f] is a float HFA");
    require(!failure_of([] { static_cast<void>(inv::parse_signature("v@:b3")); }).empty(), "bitfields fail explicitly");
    require(!failure_of([] { static_cast<void>(inv::parse_signature("v")); }).empty(), "self and _cmd are required");

    // Layout: x registers, v registers, by-reference composites, the stack.
    const auto many = inv::parse_signature("v@:qqqqqqcs{Big=qqq}fd{CGRect={CGPoint=dd}{CGSize=dd}}{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto layout = inv::assign(many);
    using W = inv::Location::Where;
    require(layout.arguments[0].where == W::x && layout.arguments[0].first == 0, "self in x0");
    require(layout.arguments[7].where == W::x && layout.arguments[7].first == 7, "sixth q in x7");
    require(layout.arguments[8].where == W::stack && layout.arguments[8].offset == 0, "c on the stack at 0");
    require(layout.arguments[9].where == W::stack && layout.arguments[9].offset == 2, "s packed at its alignment (Apple)");
    require(layout.arguments[10].where == W::reference && layout.arguments[10].stack_pointer && layout.arguments[10].offset == 8,
            "a 24-byte struct goes by reference, its pointer on the stack");
    require(layout.arguments[11].where == W::v && layout.arguments[11].first == 0, "float in v0");
    require(layout.arguments[12].where == W::v && layout.arguments[12].first == 1, "double in v1");
    require(layout.arguments[13].where == W::v && layout.arguments[13].first == 2 && layout.arguments[13].count == 4, "HFA in v2-v5");
    require(layout.arguments[14].where == W::stack && layout.arguments[14].offset == 16, "an HFA that does not fit goes on the stack");
    require(layout.stack_size == 48, "stack size rounded to 16");
    const auto big = inv::assign(inv::parse_signature("{Big=qqq}@:"));
    require(big.indirect_result, "a 24-byte result goes through x8");
    const auto pair = inv::assign(inv::parse_signature("v@:q{P=qq}"));
    require(pair.arguments[3].where == W::x && pair.arguments[3].first == 3 && pair.arguments[3].count == 2, "16-byte struct in x3-x4");
}

// ---- the undo stack --------------------------------------------------------------------------------

void test_undo_stack() {
    undo::Stack stack;
    stack.begin_group();
    require(stack.add({1, 10}).empty(), "first registration clears nothing");
    stack.begin_group();
    static_cast<void>(stack.add({2, 11}));
    stack.set_name("Inner");
    require(stack.end_group(), "inner group closes");
    stack.set_name("Move");
    require(stack.level() == 1 && stack.end_group() && stack.level() == 0, "outer group closes");
    require(stack.undo_count() == 1 && stack.undo_groups().back().actions.size() == 2, "nested groups flatten into one undo group");
    require(stack.undo_name() == "Move", "the outermost name wins");
    stack.begin_group();
    require(stack.end_group() && stack.undo_count() == 1, "an empty group is dropped");

    auto group = stack.take_undo();
    require(group && group->actions[0].id == 10 && group->actions[1].id == 11, "undo takes the newest group");
    stack.begin_perform(undo::Stack::Mode::undoing, group->name);
    require(stack.add({1, 20}).empty(), "registration while undoing does not clear");
    stack.end_perform();
    require(stack.can_redo() && stack.redo_name() == "Move" && !stack.can_undo(), "undo registrations form the redo group");

    auto redo = stack.take_redo();
    stack.begin_perform(undo::Stack::Mode::redoing, redo->name);
    static_cast<void>(stack.add({1, 30}));
    stack.end_perform();
    require(stack.can_undo() && !stack.can_redo(), "redo registrations go back to the undo stack");

    // A new registration outside undo/redo clears the redo stack.
    group = stack.take_undo();
    stack.begin_perform(undo::Stack::Mode::undoing, group->name);
    static_cast<void>(stack.add({1, 40}));
    stack.end_perform();
    stack.begin_group();
    const auto cleared = stack.add({3, 50});
    require(cleared.size() == 1 && cleared[0].id == 40 && !stack.can_redo(), "new registration clears redo");
    static_cast<void>(stack.end_group());

    // levelsOfUndo drops the oldest groups; removing a target drops empty groups.
    for (std::uint64_t id = 60; id < 64; ++id) { stack.begin_group(); static_cast<void>(stack.add({4, id})); static_cast<void>(stack.end_group()); }
    std::vector<undo::Action> dropped;
    stack.set_levels(2, &dropped);
    require(stack.undo_count() == 2 && dropped.size() == 3, "levelsOfUndo keeps the newest two");
    const auto removed = stack.remove_target(4);
    require(removed.size() == 2 && stack.undo_count() == 0, "removing a target removes its groups");
    stack.disable();
    require(!stack.registration_enabled() && stack.enable() && !stack.enable(), "disable/enable nest and fail when unbalanced");
}

// ---- providers over a reference-engine process -----------------------------------------------------

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};
    std::shared_ptr<completion_kit::Support> support;

    Fixture() {
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_runloop(context);
        alchemy::gui::install_system_configuration(context);
        alchemy::gui::install_security(context);
        alchemy::gui::install_network(context);
        support = std::make_shared<completion_kit::Support>(process, objc, cf);
        completion_kit::install_dispatch_objects(*support);
        completion_kit::install_exceptions(*support);
        completion_kit::install_threads(*support);
        completion_kit::install_locks(*support);
        completion_kit::install_invocations(*support);
        completion_kit::install_undo_manager(*support);
        completion_kit::install_key_value_coding(*support);
        completion_kit::install_collections(*support);
        completion_kit::install_key_value_observing(*support);
        completion_kit::install_bindings(*support);
        completion_kit::install_kvc_collections(*support);
        completion_kit::install_notification_queue(*support);
        completion_kit::install_value_transformers(*support);
        completion_kit::install_process_io(*support);
        completion_kit::install_streams(*support);      // NSInputStream, NSOutputStream
        completion_kit::install_files(*support);
        completion_kit::install_bundles(*support);
        completion_kit::install_os_log(*support);
        completion_kit::install_system_functions(*support);
        completion_kit::install_os_log_store(*support);
        completion_kit::install_scripting(*support);
        completion_kit::install_xpc(*support);
        completion_kit::install_foundation(*support);   // NSCache
        completion_kit::install_cf_extras(*support);
        completion_kit::install_formatters(*support);   // NSDateFormatter
    }
    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing symbol " + std::string(name));
        return found->address;
    }
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return engine.invoke_callback(symbol(library, name), std::span(arguments.begin(), arguments.size()));
    }
    Address libsystem(std::string_view name, std::initializer_list<Address> arguments = {}) { return call("/usr/lib/libSystem.B.dylib", name, arguments); }
    Address cls(std::string_view name) { const auto value = objc.find_class(name); require(value != 0, "missing class " + std::string(name)); return value; }
    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        alchemy::CallRegs registers;
        std::size_t index = 2;
        for (const auto argument : arguments) registers.x[index++] = argument;
        return objc.send(receiver, selector, registers).x0;
    }
    Address c_string(std::string_view text) {
        const auto address = process.allocate_guest(text.size() + 1);
        engine.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        static_cast<GuestMemory&>(engine).write8(address + text.size(), 0);
        return address;
    }
    std::string text(Address string) { return string ? cf.utf8(string) : std::string("(nil)"); }
    // A compiler-emitted (global) block whose invoke is `invoke`.
    Address block(Address invoke) {
        const auto value = process.allocate_guest(32), descriptor = process.allocate_guest(16);
        engine.write64(descriptor, 0);
        engine.write64(descriptor + 8, 32);
        engine.write64(value, symbol("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock"));
        engine.write32(value + 8, 1U << 28);
        engine.write32(value + 12, 0);
        engine.write64(value + 16, invoke);
        engine.write64(value + 24, descriptor);
        return value;
    }
    Address data(std::string_view bytes) {
        const auto buffer = process.allocate_guest(bytes.size() + 1);
        engine.write_bytes(buffer, std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        return cf.invoke("CFDataCreate", std::array<Address, 3>{0, buffer, bytes.size()});
    }
    std::string bytes(Address data_object) {
        std::string result(static_cast<std::size_t>(send(data_object, "length")), '\0');
        if (!result.empty()) engine.read_bytes(send(data_object, "bytes"), std::span(reinterpret_cast<std::uint8_t*>(result.data()), result.size()));
        return result;
    }
};

void test_exceptions(Fixture& f) {
    const auto name = f.cf.string("NSRangeException"), reason = f.cf.string("index 3 beyond bounds");
    const auto exception = f.send(f.cls("NSException"), "exceptionWithName:reason:userInfo:", {name, reason, 0});
    require(f.text(f.send(exception, "name")) == "NSRangeException" && f.text(f.send(exception, "reason")) == "index 3 beyond bounds",
            "name and reason");
    require(f.send(exception, "callStackSymbols") == 0, "no call stack before raising");
    const auto message = failure_of([&] { f.send(exception, "raise"); });
    require(message.find("NSRangeException: index 3 beyond bounds") != std::string::npos && message.starts_with("unsupported:"),
            "-raise fails explicitly with the name and reason: " + message);
    const auto constant = f.process.resolve(alchemy::core_foundation_library, "_NSInternalInconsistencyException");
    require(constant && f.text(f.engine.read64(constant->address)) == "NSInternalInconsistencyException", "exception name export");
    const auto ehtype = f.process.resolve(alchemy::core_foundation_library, "_OBJC_EHTYPE_$_NSException");
    require(ehtype && f.engine.read64(ehtype->address + 16) == f.cls("NSException"), "OBJC_EHTYPE_$_NSException names the class");
}

struct Recorder {
    std::int64_t integer{};
    double real{}, rect[4]{};
    Address object{};
    std::vector<std::string> calls;
};

Address make_target_class(Fixture& f, Recorder& r) {
    const auto cls = f.objc.register_native_class("G2TestTarget", f.cls("NSObject"), 16);
    f.support->method(cls, "sum:and:", [&r](CpuState& s, GuestMemory&) {
        r.calls.push_back("sum");
        s.x[0] = s.x[2] + s.x[3];
    }, "q@:qq");
    f.support->method(cls, "take:real:rect:", [&r](CpuState& s, GuestMemory&) {
        r.calls.push_back("take");
        r.integer = static_cast<std::int32_t>(s.x[2]);
        r.real = std::bit_cast<double>(s.v[0][0]);
        for (int i = 0; i < 4; ++i) r.rect[i] = std::bit_cast<double>(s.v[1 + i][0]);
        s.v[0] = {std::bit_cast<std::uint64_t>(r.real * 2), 0};
    }, "d@:id{CGRect={CGPoint=dd}{CGSize=dd}}");
    f.support->method(cls, "setLevel:", [&r](CpuState& s, GuestMemory&) {
        r.calls.push_back("setLevel:" + std::to_string(static_cast<std::int64_t>(s.x[2])));
        r.integer = static_cast<std::int64_t>(s.x[2]);
    }, "v@:q");
    f.support->method(cls, "setObject:", [&r](CpuState& s, GuestMemory&) {
        r.calls.push_back("setObject:");
        r.object = s.x[2];
    }, "v@:@");
    return cls;
}

void test_invocations(Fixture& f, Address target_class, Recorder& r) {
    const auto target = f.send(f.send(target_class, "alloc"), "init");
    const auto signature = f.send(target_class, "instanceMethodSignatureForSelector:", {f.objc.selector("sum:and:")});
    require(signature != 0, "instanceMethodSignatureForSelector:");
    require(f.send(signature, "numberOfArguments") == 4 && f.send(signature, "methodReturnLength") == 8, "signature counts");
    require(static_cast<GuestMemory&>(f.engine).read8(f.send(signature, "getArgumentTypeAtIndex:", {2})) == 'q', "argument type string");
    const auto invocation = f.send(f.cls("NSInvocation"), "invocationWithMethodSignature:", {signature});
    const auto buffer = f.process.allocate_guest(64);
    f.send(invocation, "setTarget:", {target});
    f.send(invocation, "setSelector:", {f.objc.selector("sum:and:")});
    f.engine.write64(buffer, 40);
    f.send(invocation, "setArgument:atIndex:", {buffer, 2});
    f.engine.write64(buffer, 2);
    f.send(invocation, "setArgument:atIndex:", {buffer, 3});
    f.send(invocation, "invoke");
    f.send(invocation, "getReturnValue:", {buffer});
    require(f.engine.read64(buffer) == 42, "invoke returns 40 + 2");

    // An int, a double and an HFA: x2, v0 and v1-v4; a double result in v0.
    const auto float_signature = f.send(target, "methodSignatureForSelector:", {f.objc.selector("take:real:rect:")});
    const auto call = f.send(f.cls("NSInvocation"), "invocationWithMethodSignature:", {float_signature});
    f.send(call, "setSelector:", {f.objc.selector("take:real:rect:")});
    f.engine.write32(buffer, static_cast<std::uint32_t>(-5));
    f.send(call, "setArgument:atIndex:", {buffer, 2});
    f.engine.write64(buffer, std::bit_cast<std::uint64_t>(1.5));
    f.send(call, "setArgument:atIndex:", {buffer, 3});
    for (int i = 0; i < 4; ++i) f.engine.write64(buffer + 8 * i, std::bit_cast<std::uint64_t>(10.0 + i));
    f.send(call, "setArgument:atIndex:", {buffer, 4});
    f.send(call, "invokeWithTarget:", {target});
    require(r.integer == -5 && r.real == 1.5 && r.rect[0] == 10.0 && r.rect[3] == 13.0, "int, double and CGRect arguments arrive in place");
    f.send(call, "getReturnValue:", {buffer});
    require(std::bit_cast<double>(f.engine.read64(buffer)) == 3.0, "double result");
    require(f.send(call, "target") == target, "-target after invokeWithTarget:");
    f.send(target, "release");
}

void test_undo_manager(Fixture& f, Address target_class, Recorder& r) {
    const auto manager = f.send(f.send(f.cls("NSUndoManager"), "alloc"), "init");
    const auto target = f.send(f.send(target_class, "alloc"), "init");
    f.send(manager, "setGroupsByEvent:", {0});
    require(!failure_of([&] { f.send(manager, "registerUndoWithTarget:selector:object:", {target, f.objc.selector("setObject:"), target}); }).empty(),
            "registration without a group fails when groupsByEvent is NO");
    f.send(manager, "beginUndoGrouping");
    const auto value = f.cf.string("old");
    f.send(manager, "registerUndoWithTarget:selector:object:", {target, f.objc.selector("setObject:"), value});
    const auto proxy = f.send(manager, "prepareWithInvocationTarget:", {target});
    f.send(proxy, "setLevel:", {7});
    f.send(manager, "setActionName:", {f.cf.string("Change Level")});
    f.send(manager, "endUndoGrouping");
    require(r.calls.empty() || r.calls.back() != "setLevel:7", "the proxy records instead of calling");
    require((f.send(manager, "canUndo") & 1) && !(f.send(manager, "canRedo") & 1), "canUndo after a group");
    require(f.text(f.send(manager, "undoMenuItemTitle")) == "Undo Change Level", "undo menu title");
    r.calls.clear();
    f.send(manager, "undo");
    require(r.calls.size() == 2 && r.calls[0] == "setLevel:7" && r.calls[1] == "setObject:" && r.object == value,
            "undo performs the group newest first");
    require(!(f.send(manager, "canUndo") & 1), "nothing left to undo");
    require(!(f.send(manager, "canRedo") & 1), "the actions registered nothing to redo");
    f.send(manager, "release");
    f.send(target, "release");
}

// ---- key-value observing, bindings, mutable array proxies ----

struct Model {
    std::map<Address, std::int64_t> level;
    std::map<Address, Address> child, name, items;
    std::map<Address, bool> flag;
};

struct Note { std::string path; std::int64_t kind{}; std::string new_value, old_value; bool prior{}; };

void test_key_value_observing(Fixture& f) {
    static Model model;
    static std::vector<Note> notes;
    const auto number_text = [&f](Address value) -> std::string {
        if (!value) return "absent";
        if (value == f.send(f.cls("NSNull"), "null")) return "null";
        if (f.cf.kind(value) == alchemy::ObjectKind::number) return std::to_string(static_cast<std::int64_t>(f.send(value, "longLongValue")));
        return f.text(f.send(value, "description"));
    };
    const auto model_class = f.objc.register_native_class("G2Model", f.cls("NSObject"), 16);
    f.support->method(model_class, "level", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(model.level[s.x[0]]); }, "q@:");
    f.support->method(model_class, "setLevel:", [](CpuState& s, GuestMemory&) { model.level[s.x[0]] = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    f.support->method(model_class, "child", [](CpuState& s, GuestMemory&) { s.x[0] = model.child[s.x[0]]; }, "@@:");
    f.support->method(model_class, "setChild:", [](CpuState& s, GuestMemory&) { model.child[s.x[0]] = s.x[2]; }, "v@:@");
    f.support->method(model_class, "name", [](CpuState& s, GuestMemory&) { s.x[0] = model.name[s.x[0]]; }, "@@:");
    f.support->method(model_class, "setName:", [](CpuState& s, GuestMemory&) { model.name[s.x[0]] = s.x[2]; }, "v@:@");
    f.support->method(model_class, "flag", [](CpuState& s, GuestMemory&) { s.x[0] = model.flag[s.x[0]]; }, "B@:");
    f.support->method(model_class, "setFlag:", [](CpuState& s, GuestMemory&) { model.flag[s.x[0]] = (s.x[2] & 0xff) != 0; }, "v@:B");
    f.support->method(model_class, "items", [](CpuState& s, GuestMemory&) { s.x[0] = model.items[s.x[0]]; }, "@@:");
    f.support->method(model_class, "setItems:", [&f](CpuState& s, GuestMemory&) { model.items[s.x[0]] = f.send(s.x[2], "copy"); }, "v@:@");
    f.support->method(model_class, "doubled", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(2 * model.level[s.x[0]]); }, "q@:");
    f.support->method(model_class, "keyPathsForValuesAffectingDoubled", [&f](CpuState& s, GuestMemory&) {
        s.x[0] = f.send(f.cls("NSSet"), "setWithObject:", {f.cf.string("level")});
    }, "@@:", true);
    const auto observer_class = f.objc.register_native_class("G2Observer", f.cls("NSObject"), 16);
    f.support->method(observer_class, "observeValueForKeyPath:ofObject:change:context:", [&f, number_text](CpuState& s, GuestMemory&) {
        Note note;
        note.path = f.text(s.x[2]);
        const auto get = [&](const char* key) { return f.send(s.x[4], "objectForKey:", {f.cf.string(key)}); };
        note.kind = static_cast<std::int64_t>(f.send(get("kind"), "longLongValue"));
        note.new_value = number_text(get("new"));
        note.old_value = number_text(get("old"));
        note.prior = get("notificationIsPrior") != 0;
        notes.push_back(note);
    }, "v@:@@@^v");

    const auto root = f.send(f.send(model_class, "alloc"), "init");
    const auto observer = f.send(f.send(observer_class, "alloc"), "init");
    const auto string = [&f](const char* text) { return f.cf.string(text); };
    // New | Old | Prior | Initial on a plain key.
    f.send(root, "addObserver:forKeyPath:options:context:", {observer, string("level"), 1 | 2 | 4 | 8, 0x77});
    require(notes.size() == 1 && notes[0].kind == 1 && notes[0].new_value == "0" && notes[0].old_value == "absent", "initial notification");
    require(f.objc.object_class(root) != model_class && f.send(root, "class") == model_class,
            "an observed object is an NSKVONotifying_ subclass that answers its class");
    notes.clear();
    f.send(root, "setLevel:", {5});
    require(model.level[root] == 5, "the original setter ran");
    require(notes.size() == 2 && notes[0].prior && notes[0].old_value == "0" && !notes[1].prior &&
            notes[1].new_value == "5" && notes[1].old_value == "0", "prior then change notification, with old and new values");
    notes.clear();
    f.send(root, "setValue:forKey:", {f.cf.integer(6), string("level")});
    require(notes.size() == 2 && notes[1].new_value == "6", "setValue:forKey: notifies through the overridden setter");
    // Nested will/did pairs pair up innermost first.
    notes.clear();
    f.send(root, "willChangeValueForKey:", {string("level")});
    f.send(root, "willChangeValueForKey:", {string("name")});
    f.send(root, "didChangeValueForKey:", {string("name")});
    f.send(root, "didChangeValueForKey:", {string("level")});
    require(notes.size() == 2 && notes[0].prior && !notes[1].prior, "manual will/did pair for an observed key");
    f.send(root, "removeObserver:forKeyPath:", {observer, string("level")});
    notes.clear();
    f.send(root, "setLevel:", {9});
    require(notes.empty(), "no notification after removal");
    require(!failure_of([&] { f.send(root, "removeObserver:forKeyPath:", {observer, string("level")}); }).empty(),
            "removing an unregistered observer fails explicitly");

    // Key paths re-attach when an intermediate value changes.
    const auto first = f.send(f.send(model_class, "alloc"), "init"), second = f.send(f.send(model_class, "alloc"), "init");
    f.send(first, "setLevel:", {1});
    f.send(second, "setLevel:", {2});
    f.send(root, "addObserver:forKeyPath:options:context:", {observer, string("child.level"), 1, 0});
    f.send(root, "setChild:", {first});
    require(notes.size() == 1 && notes[0].path == "child.level" && notes[0].new_value == "1", "setting the intermediate key notifies the path");
    f.send(first, "setLevel:", {11});
    require(notes.size() == 2 && notes[1].new_value == "11", "a change deep in the path notifies");
    f.send(root, "setChild:", {second});
    f.send(first, "setLevel:", {12});
    require(notes.size() == 3 && notes[2].new_value == "2", "the old intermediate object is no longer observed");
    f.send(second, "setLevel:", {22});
    require(notes.size() == 4 && notes[3].new_value == "22", "the new intermediate object is observed");
    f.send(root, "removeObserver:forKeyPath:", {observer, string("child.level")});

    // Dependent keys.
    notes.clear();
    f.send(root, "addObserver:forKeyPath:options:context:", {observer, string("doubled"), 1, 0});
    f.send(root, "setLevel:", {21});
    require(notes.size() == 1 && notes[0].path == "doubled" && notes[0].new_value == "42", "keyPathsForValuesAffecting<Key> dependency");
    f.send(root, "removeObserver:forKeyPath:", {observer, string("doubled")});

    // Bindings: model -> receiver, the reverse SPI, NSNegateBoolean.
    const auto view = f.send(f.send(model_class, "alloc"), "init");
    f.send(root, "setName:", {string("cue 1")});
    f.send(view, "bind:toObject:withKeyPath:options:", {string("name"), root, string("name"), 0});
    require(f.text(model.name[view]) == "cue 1", "bind: sets the initial value");
    f.send(root, "setName:", {string("cue 2")});
    require(f.text(model.name[view]) == "cue 2", "a bound value follows the model");
    const auto info = f.send(view, "infoForBinding:", {string("name")});
    require(f.send(info, "objectForKey:", {string("NSObservedObject")}) == root &&
            f.text(f.send(info, "objectForKey:", {string("NSObservedKeyPath")})) == "name", "infoForBinding:");
    f.send(view, "_alchemyPropagateValue:forBinding:", {string("edited"), string("name")});
    require(f.text(model.name[root]) == "edited", "the view-to-model SPI sets the observed key path");
    f.send(view, "unbind:", {string("name")});
    f.send(root, "setName:", {string("cue 3")});
    require(f.text(model.name[view]) == "edited", "unbind: stops updates");
    const auto negate = f.send(f.cls("NSDictionary"), "dictionaryWithObject:forKey:", {string("NSNegateBoolean"), string("NSValueTransformerName")});
    f.send(view, "bind:toObject:withKeyPath:options:", {string("flag"), root, string("flag"), negate});
    require(model.flag[view], "NSNegateBoolean: NO -> YES");
    f.send(root, "setFlag:", {1});
    require(!model.flag[view], "NSNegateBoolean: YES -> NO");
    f.send(view, "unbind:", {string("flag")});

    // -mutableArrayValueForKey: through -set<Key>: notifies the key's observers.
    f.send(root, "setItems:", {f.cf.array({})});
    notes.clear();
    f.send(root, "addObserver:forKeyPath:options:context:", {observer, string("items"), 1, 0});
    const auto proxy = f.send(root, "mutableArrayValueForKey:", {string("items")});
    f.send(proxy, "addObject:", {string("a")});
    f.send(proxy, "addObject:", {string("b")});
    f.send(proxy, "removeObjectAtIndex:", {0});
    require(f.send(model.items[root], "count") == 1 && f.text(f.send(model.items[root], "objectAtIndex:", {0})) == "b", "proxy edits reach -setItems:");
    require(notes.size() == 3 && f.send(proxy, "count") == 1, "each proxy edit notifies the observers of items");
    f.send(root, "removeObserver:forKeyPath:", {observer, string("items")});
}

// ---- file handles, pipes, notification queues, operations ----

std::string guest_path_of(const std::filesystem::path& host) {
    auto text = host.generic_string();
    return "/Volumes/" + text.substr(0, 1) + text.substr(2);
}

void test_notification_queue(Fixture& f) {
    static int posted;
    posted = 0;
    const auto observer_class = f.objc.register_native_class("G2NotificationObserver", f.cls("NSObject"), 16);
    f.support->method(observer_class, "note:", [](CpuState&, GuestMemory&) { ++posted; }, "v@:@");
    const auto observer = f.send(f.send(observer_class, "alloc"), "init");
    const auto center = f.send(f.cls("NSNotificationCenter"), "defaultCenter");
    const auto name = f.cf.string("G2Note");
    f.send(center, "addObserver:selector:name:object:", {observer, f.objc.selector("note:"), name, 0});
    const auto queue = f.send(f.cls("NSNotificationQueue"), "defaultQueue");
    require(queue == f.send(f.cls("NSNotificationQueue"), "defaultQueue"), "one default queue per thread");
    const auto note = f.send(f.cls("NSNotification"), "notificationWithName:object:", {name, 0});
    f.send(queue, "enqueueNotification:postingStyle:", {note, 2});   // NSPostASAP
    f.send(queue, "enqueueNotification:postingStyle:", {note, 2});   // coalesced
    require(posted == 0, "ASAP notifications wait for the run loop");
    f.send(queue, "enqueueNotification:postingStyle:coalesceMask:forModes:", {note, 3, 1, 0});   // NSPostNow, coalescing on name
    require(posted == 1, "NSPostNow posts at once and removes the coalesced ASAP one");
    f.send(queue, "enqueueNotification:postingStyle:", {note, 2});
    const auto mode = f.engine.read64(f.symbol(alchemy::core_foundation_library, "_kCFRunLoopDefaultMode"));
    alchemy::CallRegs run;
    run.x[0] = mode;
    run.set_d(0, 0.05);
    run.x[1] = 1;
    static_cast<void>(f.support->call(alchemy::core_foundation_library, "_CFRunLoopRunInMode", run));
    require(posted == 2, "the run loop posts the ASAP notification: " + std::to_string(posted));
    f.send(center, "removeObserver:", {observer});
}

// value_transformers.cpp: the name registry (with lazy registration by class
// name), the defaults a subclass inherits, the built-in transformers, and
// NSFileCoordinator running accessors at once.
void test_value_transformers(Fixture& f) {
    const auto base = f.cls("NSValueTransformer");
    const auto upper = f.objc.register_native_class("G2UpperTransformer", base, 8);
    static Address transformed_marker;
    transformed_marker = f.cf.string("transformed");
    f.support->method(upper, "transformedValue:", [](CpuState& s, GuestMemory&) { s.x[0] = transformed_marker; }, "@@:@");
    const auto value = f.cf.string("value");
    const auto name = f.cf.string("G2Upper");
    require(f.send(base, "valueTransformerForName:", {name}) == 0, "an unknown name has no transformer");
    const auto transformer = f.send(f.send(upper, "alloc"), "init");
    f.send(base, "setValueTransformer:forName:", {transformer, name});
    require(f.send(base, "valueTransformerForName:", {name}) == transformer, "registered transformer");
    require(f.send(transformer, "reverseTransformedValue:", {value}) == transformed_marker, "reverse goes through the subclass's transformedValue:");
    require(f.send(upper, "transformedValueClass") == f.cls("NSObject") && (f.send(upper, "allowsReverseTransformation") & 0xff) == 1,
            "inherited class defaults");
    const auto plain = f.send(f.send(base, "alloc"), "init");
    require(f.send(plain, "transformedValue:", {value}) == value, "the default transformedValue: returns its argument");
    const auto names = f.send(base, "valueTransformerNames");
    require(f.send(names, "containsObject:", {name}) == 1 && f.send(names, "containsObject:", {f.cf.string("NSNegateBoolean")}) == 1,
            "valueTransformerNames");
    f.send(base, "setValueTransformer:forName:", {0, name});
    require(f.send(base, "valueTransformerForName:", {name}) == 0, "a nil transformer removes the name");
    const auto by_class = f.send(base, "valueTransformerForName:", {f.cf.string("G2UpperTransformer")});
    require(by_class && f.objc.object_class(by_class) == upper &&
            f.send(base, "valueTransformerForName:", {f.cf.string("G2UpperTransformer")}) == by_class, "a subclass name registers an instance");
    require(f.send(base, "valueTransformerForName:", {f.cf.string("NSString")}) == 0, "a class that is not a transformer is not used");
    const auto negate = f.send(base, "valueTransformerForName:", {f.cf.string("NSNegateBoolean")});
    const auto yes = f.send(f.cls("NSNumber"), "numberWithBool:", {1});
    require((f.send(f.send(negate, "transformedValue:", {yes}), "boolValue") & 0xff) == 0 &&
            (f.send(f.send(negate, "reverseTransformedValue:", {0}), "boolValue") & 0xff) == 1, "NSNegateBoolean");
    const auto is_nil = f.send(base, "valueTransformerForName:", {f.engine.read64(f.symbol(alchemy::foundation_library, "_NSIsNilTransformerName"))});
    require((f.send(f.send(is_nil, "transformedValue:", {0}), "boolValue") & 0xff) == 1 &&
            (f.send(f.send(is_nil, "transformedValue:", {value}), "boolValue") & 0xff) == 0, "NSIsNil");
    bool refused{};
    try { static_cast<void>(f.send(is_nil, "reverseTransformedValue:", {value})); }
    catch (const std::exception& error) { refused = std::string(error.what()).find("does not allow reverse") != std::string::npos; }
    require(refused, "NSIsNil cannot reverse");
    // NSFileCoordinator: the accessor runs at once with the URL.
    static Address seen;
    seen = 0;
    const auto invoke = f.process.install_function("_g2_test_coordinator_block", [](CpuState& s, GuestMemory&) { seen = s.x[1]; s.x[0] = 0; });
    const auto coordinator = f.send(f.send(f.cls("NSFileCoordinator"), "alloc"), "initWithFilePresenter:", {0});
    const auto url = f.cf.file_url("/tmp/coordinated", false);
    const auto error = f.process.allocate_guest(8);
    f.engine.write64(error, 1);
    f.send(coordinator, "coordinateWritingItemAtURL:options:error:byAccessor:", {url, 0, error, f.block(invoke)});
    require(seen == url && f.engine.read64(error) == 0, "NSFileCoordinator runs the accessor with the URL and no error");
}

void test_operations(Fixture& f) {
    static std::atomic<int> ran;
    ran = 0;
    const auto invoke = f.process.install_function("_g2_test_operation_block", [](CpuState& s, GuestMemory&) { ++ran; s.x[0] = 0; });
    const auto block = f.block(invoke);
    const auto first = f.send(f.cls("NSBlockOperation"), "blockOperationWithBlock:", {block});
    const auto second = f.send(f.cls("NSBlockOperation"), "blockOperationWithBlock:", {block});
    f.send(second, "addDependency:", {first});
    require(!(f.send(second, "isReady") & 1), "an operation with an unfinished dependency is not ready");
    const auto queue = f.send(f.send(f.cls("NSOperationQueue"), "alloc"), "init");
    f.send(queue, "addOperation:", {second});
    f.send(queue, "addOperation:", {first});
    f.send(queue, "waitUntilAllOperationsAreFinished");
    require(ran == 2 && (f.send(first, "isFinished") & 1) && (f.send(second, "isFinished") & 1), "both operations ran: " + std::to_string(ran.load()));
    require(f.send(queue, "operationCount") == 0, "the queue is empty afterwards");
    const auto cancelled = f.send(f.cls("NSBlockOperation"), "blockOperationWithBlock:", {block});
    f.send(cancelled, "cancel");
    f.send(cancelled, "start");
    require(ran == 2 && (f.send(cancelled, "isFinished") & 1) && (f.send(cancelled, "isCancelled") & 1), "a cancelled operation finishes without running");
}

void test_file_manager(Fixture& f) {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "alchemy_g2_files";
    fs::remove_all(base);
    fs::create_directories(base / "a" / "sub");
    fs::create_directories(base / "b" / "sub");
    fs::create_directories(base / "a" / "skip" / "deep");
    for (const auto* side : {"a", "b"}) {
        std::ofstream(base / side / "one.txt", std::ios::binary) << "same";
        std::ofstream(base / side / "sub" / "two.txt", std::ios::binary) << "contents";
    }
    std::ofstream(base / "a" / ".hidden", std::ios::binary) << "h";
    std::ofstream(base / "a" / "skip" / "deep" / "x", std::ios::binary) << "x";
    const auto manager = f.send(f.cls("NSFileManager"), "defaultManager");
    const auto path = [&](const fs::path& value) { return f.cf.string(guest_path_of(value)); };

    // Pre-order enumeration with -skipDescendants and -level.
    const auto enumerator = f.send(manager, "enumeratorAtPath:", {path(base / "a")});
    std::vector<std::string> seen;
    for (auto value = f.send(enumerator, "nextObject"); value; value = f.send(enumerator, "nextObject")) {
        const auto name = f.text(value);
        seen.push_back(name + "@" + std::to_string(f.send(enumerator, "level")));
        if (name == "skip") f.send(enumerator, "skipDescendants");
    }
    const std::vector<std::string> expected{".hidden@1", "one.txt@1", "skip@1", "sub@1", "sub/two.txt@2"};
    require(seen == expected, "enumeratorAtPath: pre-order with skipDescendants");
    const auto subpaths = f.send(manager, "subpathsOfDirectoryAtPath:error:", {path(base / "b"), 0});
    require(f.send(subpaths, "count") == 3, "subpathsOfDirectoryAtPath:error:");
    // URL enumeration skipping hidden files.
    const auto url = f.send(f.cls("NSURL"), "fileURLWithPath:", {path(base / "a")});
    const auto contents = f.send(manager, "contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:", {url, 0, 4, 0});
    require(f.send(contents, "count") == 3, "contentsOfDirectoryAtURL: skips hidden files");

    // Contents equality, recursive.
    require(f.send(manager, "contentsEqualAtPath:andPath:", {path(base / "a" / "sub"), path(base / "b" / "sub")}) & 1, "equal directories");
    require(!(f.send(manager, "contentsEqualAtPath:andPath:", {path(base / "a"), path(base / "b")}) & 1), "different directories");
    const auto data = f.send(manager, "contentsAtPath:", {path(base / "a" / "one.txt")});
    require(f.bytes(data) == "same", "contentsAtPath:");

    // Symbolic links and replacement.
    // Host symbolic links need the Windows privilege (or developer mode); without it the error path is checked.
    const auto error_cell = f.process.allocate_guest(8);
    f.engine.write64(error_cell, 0);
    if (f.send(manager, "createSymbolicLinkAtPath:withDestinationPath:error:", {path(base / "link"), f.cf.string("a/one.txt"), error_cell}) & 1)
        require(f.text(f.send(manager, "destinationOfSymbolicLinkAtPath:error:", {path(base / "link"), 0})) == "a/one.txt", "readlink");
    else
        require(f.engine.read64(error_cell) != 0 && f.text(f.send(f.engine.read64(error_cell), "domain")) == "NSCocoaErrorDomain",
                "a failed symlink reports an NSCocoaErrorDomain error");
    std::ofstream(base / "new.txt", std::ios::binary) << "replacement";
    const auto original = f.send(f.cls("NSURL"), "fileURLWithPath:", {path(base / "a" / "one.txt")});
    const auto replacement = f.send(f.cls("NSURL"), "fileURLWithPath:", {path(base / "new.txt")});
    const auto result_cell = f.process.allocate_guest(8);
    require(f.send(manager, "replaceItemAtURL:withItemAtURL:backupItemName:options:resultingItemURL:error:",
                   {original, replacement, f.cf.string("one.bak"), 2, result_cell, 0}) & 1, "replaceItemAtURL:");
    std::ifstream replaced(base / "a" / "one.txt", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(replaced)), {});
    require(text == "replacement" && fs::exists(base / "a" / "one.bak") && !fs::exists(base / "new.txt"), "replacement in place with a kept backup");
    // Relationship.
    const auto cell = f.process.allocate_guest(8);
    const auto parent = f.send(f.cls("NSURL"), "fileURLWithPath:", {path(base)});
    require((f.send(manager, "getRelationship:ofDirectoryAtURL:toItemAtURL:error:", {cell, parent, original, 0}) & 1) && f.engine.read64(cell) == 0,
            "the directory contains the item");
    replaced.close();
    fs::remove_all(base);
}

void test_scripting_definition() {
    namespace sdef = alchemy::gui::completion_kit::sdef;
    const auto parsed = sdef::parse(R"(<?xml version="1.0"?>
<!DOCTYPE dictionary SYSTEM "file://localhost/System/Library/DTDs/sdef.dtd">
<dictionary>
  <suite name="QLab Suite" code="QLab">
    <!-- a comment with <tags> -->
    <class name="workspace" code="QWsp" plural="workspaces" inherits="document">
      <cocoa class="QLabWorkspace"/>
      <property name="q number" code="qNum" type="text" access="r"/>
      <property name="unique ID" code="ID  " type="text"><cocoa key="uniqueID"/></property>
      <element type="cue"><cocoa key="cuesForScripting"/></element>
      <responds-to command="go"/>
    </class>
    <class name="cue" code="QCue" plural="cues"><cocoa class="QLabCue"/></class>
    <class-extension extends="workspace"><property name="active &amp; ready" code="actv" type="boolean"/></class-extension>
    <command name="go" code="QLabGO  "><cocoa class="QLabGoCommand"/></command>
  </suite>
</dictionary>)");
    require(parsed.has_value(), "sdef parses");
    const auto* workspace = parsed->by_cocoa_class("QLabWorkspace");
    require(workspace && workspace->name == "workspace" && workspace->suite == "QLab Suite", "class by Cocoa class");
    require(workspace->properties.size() == 3 && workspace->properties[0].key == "qNumber" && workspace->properties[1].key == "uniqueID" &&
            workspace->properties[2].name == "active & ready", "property keys, cocoa keys, entities, class extensions");
    require(workspace->elements.size() == 1 && workspace->elements[0].key == "cuesForScripting", "element key");
    require(parsed->commands.size() == 1 && parsed->commands[0].cocoa_class == "QLabGoCommand", "command");
    require(sdef::four_char_code("QLab") == 0x514c6162, "four character codes");
    require(!sdef::parse("<dictionary><suite name=\"x>").has_value(), "malformed XML is rejected");
}

void test_scripting(Fixture& f) {
    // Specifiers evaluate through key-value coding.
    static Model holder;
    const auto container_class = f.objc.register_native_class("G2Container", f.cls("NSObject"), 16);
    const auto item_class = f.objc.register_native_class("G2Item", f.cls("NSObject"), 16);
    static std::map<Address, Address> ids;
    f.support->method(item_class, "uniqueID", [](CpuState& s, GuestMemory&) { s.x[0] = ids[s.x[0]]; }, "@@:");
    f.support->method(container_class, "items", [](CpuState& s, GuestMemory&) { s.x[0] = holder.items[s.x[0]]; }, "@@:");
    const auto container = f.send(f.send(container_class, "alloc"), "init");
    const auto first = f.send(f.send(item_class, "alloc"), "init"), second = f.send(f.send(item_class, "alloc"), "init");
    ids[first] = f.cf.string("A");
    ids[second] = f.cf.string("B");
    const std::array<Address, 2> both{first, second};
    holder.items[container] = f.cf.array(both);
    const auto found = f.send(container, "valueWithUniqueID:inPropertyWithKey:", {f.cf.string("B"), f.cf.string("items")});
    require(found == second, "valueWithUniqueID:inPropertyWithKey:");
    const auto unique = f.send(f.send(f.cls("NSUniqueIDSpecifier"), "alloc"), "initWithContainerClassDescription:containerSpecifier:key:uniqueID:",
                               {0, 0, f.cf.string("items"), f.cf.string("A")});
    require(f.send(unique, "objectsByEvaluatingWithContainers:", {container}) == first, "NSUniqueIDSpecifier evaluation");
    require(f.text(f.send(unique, "uniqueID")) == "A" && f.text(f.send(unique, "key")) == "items", "specifier accessors");
    const auto property = f.send(f.send(f.cls("NSPropertySpecifier"), "alloc"), "initWithContainerClassDescription:containerSpecifier:key:",
                                 {0, 0, f.cf.string("items")});
    require(f.send(f.send(property, "objectsByEvaluatingWithContainers:", {container}), "count") == 2, "NSPropertySpecifier evaluation");
    // Commands: +currentCommand during -executeCommand.
    static Address seen_current;
    const auto command_class = f.objc.register_native_class("G2DeleteCommand", f.cls("NSDeleteCommand"), 16);
    f.support->method(command_class, "performDefaultImplementation", [&f](CpuState& s, GuestMemory&) {
        seen_current = f.send(f.cls("NSScriptCommand"), "currentCommand");
        s.x[0] = 0;
    }, "@@:");
    const auto command = f.send(f.send(command_class, "alloc"), "init");
    f.send(command, "setReceiversSpecifier:", {unique});
    f.send(command, "executeCommand");
    require(seen_current == command && f.send(f.cls("NSScriptCommand"), "currentCommand") == 0, "+currentCommand while executing");
    // No application object here: the receivers specifier has no top-level container to evaluate in.
    require(f.send(command, "evaluatedReceivers") == 0 && f.send(unique, "evaluationErrorNumber") == 1, "evaluation without a container");
    f.send(command, "setScriptErrorNumber:", {static_cast<Address>(-1728)});
    require(static_cast<std::int64_t>(f.send(command, "scriptErrorNumber")) == -1728, "script error number");
    // NSAppleScript keeps its source and fails explicitly when run.
    const auto script = f.send(f.send(f.cls("NSAppleScript"), "alloc"), "initWithSource:", {f.cf.string("beep")});
    require(f.text(f.send(script, "source")) == "beep" && !(f.send(script, "isCompiled") & 1), "NSAppleScript source");
    require(failure_of([&] { f.send(script, "executeAndReturnError:", {0}); }).starts_with("unsupported:"), "AppleScript execution fails explicitly");
    // Descriptors: keyword parameters.
    const auto record = f.send(f.cls("NSAppleEventDescriptor"), "recordDescriptor");
    const auto value = f.send(f.cls("NSAppleEventDescriptor"), "descriptorWithString:", {f.cf.string("cue 1")});
    f.send(record, "setParamDescriptor:forKeyword:", {value, 0x2d2d2d2d});
    require(f.text(f.send(f.send(record, "paramDescriptorForKeyword:", {0x2d2d2d2d}), "stringValue")) == "cue 1", "paramDescriptorForKeyword:");
}

void test_version_numbers() {
    require(completion_kit::bundle_version_number("5.6.3") == 0x05638000, "5.6.3");
    require(completion_kit::bundle_version_number("10.2") == 0x10208000, "BCD major 10.2");
    require(completion_kit::bundle_version_number("1.0b7") == 0x01006007, "beta stage and revision");
    require(completion_kit::bundle_version_number("2.1.1d12") == 0x0211200c, "development stage");
    require(completion_kit::bundle_version_number("123") == 0 && completion_kit::bundle_version_number("1.22") == 0 && completion_kit::bundle_version_number("1.0x1") == 0 &&
            completion_kit::bundle_version_number("") == 0, "malformed versions are 0");
}

void test_cf_extras(Fixture& f) {
    namespace fs = std::filesystem;
    const auto lib = std::string_view("/usr/lib/libSystem.B.dylib");
    const auto cf_lib = alchemy::core_foundation_library;
    // Class identities under CoreFoundation.
    for (const auto* name : {"NSCache", "NSInputStream", "NSInvocation", "NSTimer"}) {
        const auto found = f.process.resolve(cf_lib, std::string("_OBJC_CLASS_$_") + name);
        require(found && found->address == f.objc.find_class(name), std::string(name) + " under CoreFoundation");
    }
    require(f.process.resolve(cf_lib, "_NSURLTagNamesKey").has_value(), "NSURLTagNamesKey");

    // CFFileDescriptor: a readable pipe end calls back on the run loop, once.
    static int callbacks;
    static Address types;
    callbacks = 0;
    const auto callout = f.process.install_function("_g2_test_fd_callout", [](CpuState& s, GuestMemory&) { ++callbacks; types = s.x[1]; s.x[0] = 0; });
    const auto fds = f.process.allocate_guest(8);
    require(f.call(lib, "_pipe", {fds}) == 0, "pipe");
    const auto reader = f.engine.read32(fds), writer = f.engine.read32(fds + 4);
    const auto descriptor = f.call(cf_lib, "_CFFileDescriptorCreate", {0, reader, 1, callout, 0});
    require(descriptor && f.call(cf_lib, "_CFFileDescriptorGetNativeDescriptor", {descriptor}) == reader, "CFFileDescriptorCreate");
    const auto source = f.call(cf_lib, "_CFFileDescriptorCreateRunLoopSource", {0, descriptor, 0});
    const auto mode = f.engine.read64(f.symbol(cf_lib, "_kCFRunLoopDefaultMode"));
    f.call(cf_lib, "_CFRunLoopAddSource", {f.call(cf_lib, "_CFRunLoopGetCurrent"), source, mode});
    f.call(cf_lib, "_CFFileDescriptorEnableCallBacks", {descriptor, 1});
    const auto byte = f.process.allocate_guest(8);
    f.engine.write32(byte, 'x');
    f.call(lib, "_write", {writer, byte, 1});
    const auto run = [&f, mode](double seconds) {
        alchemy::CallRegs registers;
        registers.x[0] = mode;
        registers.set_d(0, seconds);
        registers.x[1] = 1;
        static_cast<void>(f.support->call(alchemy::core_foundation_library, "_CFRunLoopRunInMode", registers));
    };
    for (int i = 0; i < 40 && callbacks == 0; ++i) run(0.05);
    require(callbacks == 1 && types == 1, "the read callback ran on the run loop");
    for (int i = 0; i < 4; ++i) run(0.02);
    require(callbacks == 1, "callbacks are one-shot until enabled again");
    f.call(cf_lib, "_CFFileDescriptorEnableCallBacks", {descriptor, 1});
    for (int i = 0; i < 40 && callbacks == 1; ++i) run(0.05);
    require(callbacks == 2, "re-enabled callback fires while data is unread");
    f.call(cf_lib, "_CFFileDescriptorInvalidate", {descriptor});
    require(!(f.call(cf_lib, "_CFFileDescriptorIsValid", {descriptor}) & 0xff), "invalidated");
    f.call(lib, "_close", {writer});
}

} // namespace

// NSDateFormatter's name arrays and templates (Blink's date fields; monthSymbols stopped a Chrome renderer).
void test_date_formatter(Fixture& f) {
    const auto formatters = f.cls("NSDateFormatter");
    const auto formatter = f.send(f.send(formatters, "alloc"), "init");
    const auto element = [&](Address array, std::uint64_t index) { return f.text(f.cf.element(array, index)); };
    const auto months = f.send(formatter, "monthSymbols");
    require(months && f.cf.length(months) == 12 && element(months, 0) == "January" && element(months, 11) == "December", "monthSymbols");
    require(element(f.send(formatter, "shortStandaloneMonthSymbols"), 8) == "Sep", "shortStandaloneMonthSymbols");
    const auto days = f.send(formatter, "shortWeekdaySymbols");
    require(days && f.cf.length(days) == 7 && element(days, 0) == "Sun" && element(days, 6) == "Sat", "shortWeekdaySymbols (Sunday first)");
    require(element(f.send(formatter, "veryShortWeekdaySymbols"), 1) == "M", "veryShortWeekdaySymbols");
    require(element(f.send(formatter, "eraSymbols"), 0) == "BC" && element(f.send(formatter, "eraSymbols"), 1) == "AD", "eraSymbols");
    require(f.text(f.send(formatters, "dateFormatFromTemplate:options:locale:", {f.cf.string("yyyyMMdd"), 0, 0})) == "MM/dd/yyyy",
            "+dateFormatFromTemplate:options:locale:");
    f.send(formatter, "setLocalizedDateFormatFromTemplate:", {f.cf.string("yMMMd")});
    require(f.text(f.send(formatter, "dateFormat")) == "MMM d, y", "-setLocalizedDateFormatFromTemplate:");
}

int main() {
    try {
        test_type_encodings();
        test_undo_stack();
        test_scripting_definition();
        test_version_numbers();
        Fixture fixture;
        Recorder recorder;
        const auto target_class = make_target_class(fixture, recorder);
        test_exceptions(fixture);
        test_invocations(fixture, target_class, recorder);
        test_undo_manager(fixture, target_class, recorder);
        test_key_value_observing(fixture);
        test_notification_queue(fixture);
        test_value_transformers(fixture);
        test_operations(fixture);
        test_file_manager(fixture);
        test_scripting(fixture);
        test_cf_extras(fixture);
        test_date_formatter(fixture);
        std::cout << "Alchemy Foundation runtime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
