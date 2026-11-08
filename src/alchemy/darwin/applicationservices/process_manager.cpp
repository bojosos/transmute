#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

#include <string>
#include <utility>
#include <vector>

// ApplicationServices (HIServices) Process Manager (headless-correct):
// GetCurrentProcess, and TransformProcessType / SetFrontProcess over the
// NSApplication activation policy; GetFrontProcess, SameProcess,
// GetProcessPID, CopyProcessName and ProcessInformationCopyDictionary for the
// current process (the only Mac process; others answer procNotFound / NULL).
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class ProcessManager final : public Provider {
public:
    using Provider::Provider;
    Address application() {
        const auto cell = context.resolve(appkit, "_NSApp");
        return cell ? memory().read64(cell->address) : 0;
    }

    void install() {
        const auto is_current = [](GuestMemory& m, Address psn) {
            return psn && m.read32(psn) == 0 && (m.read32(psn + 4) == current_process);
        };
        function(application_services, "_GetCurrentProcess", [](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            m.write32(s.x[0], 0); m.write32(s.x[0] + 4, current_process);
            s.x[0] = 0;
        });
        // The process type maps to the NSApplication activation policy.
        function(application_services, "_TransformProcessType", [this, is_current](CpuState& s, GuestMemory& m) {
            if (!is_current(m, s.x[0])) { s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            const auto type = static_cast<std::uint32_t>(s.x[1]);
            if (type < 1 || type > 4) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            if (const auto app = application()) {
                // Foreground -> Regular(0), Background -> Prohibited(2), UIElement -> Accessory(1).
                const Address policy = type == 1 ? 0 : type == 2 ? 2 : 1;
                static_cast<void>(send(app, "setActivationPolicy:", {policy}));
            }
            s.x[0] = 0;
        });
        function(application_services, "_SetFrontProcess", [this, is_current](CpuState& s, GuestMemory& m) {
            if (!is_current(m, s.x[0])) { s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            const auto app = application();
            if (!app) { s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            static_cast<void>(send(app, "activateIgnoringOtherApps:", {1}));
            s.x[0] = 0;
        });
        // The guest is the only Mac process: the front process is the current
        // one while its application is active; otherwise another (host) program
        // is in front and there is no Mac process to name (procNotFound, kNoProcess).
        // GetNextProcess walks the processes from kNoProcess: the current one is
        // the only Mac process, after it procNotFound (and kNoProcess).
        function(application_services, "_GetNextProcess", [](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            const auto high = m.read32(s.x[0]), low = m.read32(s.x[0] + 4);
            if (high == 0 && low == 0) { m.write32(s.x[0] + 4, current_process); s.x[0] = 0; return; }
            if (high == 0 && low == current_process) { m.write32(s.x[0] + 4, 0); s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            s.x[0] = static_cast<std::uint16_t>(param_err);
        });
        function(application_services, "_GetFrontProcess", [this](CpuState& s, GuestMemory& m) {
            if (!s.x[0]) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            const auto app = application();
            const bool front = app && (send(app, "isActive") & 0xff) != 0;
            m.write32(s.x[0], 0);
            m.write32(s.x[0] + 4, front ? current_process : 0);
            s.x[0] = front ? 0 : static_cast<std::uint16_t>(proc_not_found);
        });
        // OSErr SameProcess(const ProcessSerialNumber*, const ProcessSerialNumber*, Boolean*)
        function(application_services, "_SameProcess", [](CpuState& s, GuestMemory& m) {
            if (!s.x[0] || !s.x[1] || !s.x[2]) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            const bool same = m.read32(s.x[0]) == m.read32(s.x[1]) && m.read32(s.x[0] + 4) == m.read32(s.x[1] + 4);
            m.write8(s.x[2], same ? 1 : 0);
            s.x[0] = 0;
        });
        function(application_services, "_GetProcessPID", [this, is_current](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            if (!is_current(m, s.x[0])) { s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            m.write32(s.x[1], static_cast<std::uint32_t>(pid()));
            s.x[0] = 0;
        });
        function(application_services, "_CopyProcessName", [this, is_current](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) { s.x[0] = static_cast<std::uint16_t>(param_err); return; }
            if (!is_current(m, s.x[0])) { s.x[0] = static_cast<std::uint16_t>(proc_not_found); return; }
            m.write64(s.x[1], cf.string(name()));
            s.x[0] = 0;
        });
        // CFDictionaryRef ProcessInformationCopyDictionary(const ProcessSerialNumber*, UInt32): the keys this
        // process has (bundle keys only for a bundled process); NULL for any other process.
        function(application_services, "_ProcessInformationCopyDictionary", [this, is_current](CpuState& s, GuestMemory& m) {
            if (!is_current(m, s.x[0])) { s.x[0] = 0; return; }
            std::vector<std::pair<Address, Address>> entries;
            const auto add = [&](std::string_view key, Address value) { if (value) entries.emplace_back(cf.string(key), value); };
            const auto bundle = main_bundle();
            add("CFBundleName", cf.string(name()));
            if (bundle) {
                if (const auto identifier = guest_call(core_foundation, "_CFBundleGetIdentifier", {bundle})) add("CFBundleIdentifier", cf.copy(identifier));
                if (const auto executable = bundle_path(bundle, "_CFBundleCopyExecutableURL"); !executable.empty()) add("CFBundleExecutable", cf.string(executable));
                if (const auto path = bundle_path(bundle, "_CFBundleCopyBundleURL"); path.ends_with(".app")) add("BundlePath", cf.string(path));
                for (const auto* key : {"LSBackgroundOnly", "LSUIElement"}) add(key, cf.boolean(info_flag(bundle, key)));
            }
            add("pid", cf.integer(pid(), 3));   // kCFNumberSInt32Type
            add("PSN", cf.integer(current_process));
            const auto app = application();
            add("IsHiddenAttr", cf.boolean(app && (send(app, "isHidden") & 0xff) != 0));
            s.x[0] = cf.dictionary(entries);
            for (const auto& [key, value] : entries) {
                static_cast<void>(guest_call(core_foundation, "_CFRelease", {key}));
                static_cast<void>(guest_call(core_foundation, "_CFRelease", {value}));
            }
        });
    }

private:
    static constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    Address pid() { return guest_call(libsystem, "_getpid", {}) & 0xffffffff; }
    Address main_bundle() { return guest_call(core_foundation, "_CFBundleGetMainBundle", {}); }
    Address info_value(Address bundle, std::string_view key) {
        const auto name = cf.string(key);
        const auto value = guest_call(core_foundation, "_CFBundleGetValueForInfoDictionaryKey", {bundle, name});
        static_cast<void>(guest_call(core_foundation, "_CFRelease", {name}));
        return value;
    }
    bool info_flag(Address bundle, std::string_view key) {
        const auto value = info_value(bundle, key);
        if (!value) return false;
        if (cf.kind(value) == ObjectKind::string) return cf.utf8(value) == "1" || cf.utf8(value) == "YES" || cf.utf8(value) == "true";
        const auto number = cf.number_value(value);
        return number && *number != 0;
    }
    // A main bundle with no location on disk (a bare process) has no paths.
    std::string bundle_path(Address bundle, std::string_view copier) {
        Address url = 0;
        try { url = guest_call(core_foundation, copier, {bundle}); } catch (const std::exception&) { return {}; }
        if (!url) return {};
        auto path = cf.url_path(url);
        static_cast<void>(guest_call(core_foundation, "_CFRelease", {url}));
        while (path.size() > 1 && path.ends_with('/')) path.pop_back();
        return path;
    }
    // The bundle's CFBundleName, else CFBundleExecutable, else the executable's file name.
    std::string name() {
        if (const auto bundle = main_bundle()) {
            for (const auto* key : {"CFBundleName", "CFBundleExecutable"})
                if (const auto value = info_value(bundle, key); value && cf.kind(value) == ObjectKind::string && !cf.utf8(value).empty()) return cf.utf8(value);
            const auto path = bundle_path(bundle, "_CFBundleCopyExecutableURL");
            if (auto file = path.substr(path.find_last_of('/') + 1); !file.empty()) return file;
        }
        if (context.resolve(libsystem, "_getprogname")) {
            if (const auto name = guest_call(libsystem, "_getprogname", {})) {
                if (auto text = guest_text(memory(), name); !text.empty()) return text;
            }
        }
        if (const auto info = objc.find_class("NSProcessInfo")) return cf.utf8(send(send(info, "processInfo"), "processName"));
        return {};
    }

    static constexpr std::uint32_t current_process = 2; // kCurrentProcess
    static constexpr std::int32_t proc_not_found = -600, param_err = -50;
};

} // namespace

void install_applicationservices_process_manager(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<ProcessManager>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
