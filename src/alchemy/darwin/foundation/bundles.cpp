#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <map>

// NSBundle additions (the Foundation bundles provider has paths, Info.plist
// and resources) and CFBundleCopyInfoDictionaryInDirectory.
//   +bundleWithURL:, -initWithURL:
//   +bundleWithIdentifier:  the main bundle, bundles created so far, and the
//                           frameworks and plug-ins inside the main bundle
//                           (Contents/Frameworks, Contents/PlugIns); nil otherwise
//   -classNamed:            the class of that name when it comes from an
//                           image inside the bundle (dladdr), else nil
//   -principalClass         the class NSPrincipalClass names (nil without one)
//   -URLForAuxiliaryExecutable:, -pathForAuxiliaryExecutable:  Contents/MacOS/<name> when it exists
//   -pathForImageResource:, -URLForImageResource:  the image file types NSImage reads
//   -imageForResource: (AppKit)  an NSImage of that file
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

struct Bundles {
    explicit Bundles(Support& value) : support(value) {}
    Support& support;
    std::mutex mutex;
    std::map<std::string, Address> by_identifier;   // retained bundles
    bool scanned{};

    std::string path_of(Address bundle) { return text(support, support.send_id(bundle, "bundlePath")); }
    bool exists(const std::string& path) {
        GuestBlock text_path(support, path), buffer(support, 144);
        return support.call(libsystem, "_stat", {text_path, buffer}) == 0;
    }
    void remember(Address bundle) {
        if (!bundle) return;
        const auto identifier = support.send_id(bundle, "bundleIdentifier");
        if (!identifier) return;
        const auto key = text(support, identifier);
        std::lock_guard lock(mutex);
        if (by_identifier.contains(key)) return;
        by_identifier[key] = objc_retain(support, bundle);
    }
    // Frameworks and plug-ins inside the main bundle, once.
    void scan() {
        {
            std::lock_guard lock(mutex);
            if (scanned) return;
            scanned = true;
        }
        const auto main = support.send_id(class_named(support, "NSBundle"), "mainBundle");
        remember(main);
        const auto base = path_of(main);
        for (const auto* folder : {"/Contents/Frameworks", "/Contents/PlugIns", "/Contents/Library/LoginItems", "/Contents/XPCServices"}) {
            const auto directory = base + folder;
            GuestBlock text_path(support, directory);
            const auto handle = support.call(libsystem, "_opendir", {text_path});
            if (!handle) continue;
            std::vector<std::string> names;
            for (;;) {
                const auto entry = support.call(libsystem, "_readdir", {handle});
                if (!entry) break;
                const auto length = support.memory().read32(entry + 16) >> 16;
                std::string name(length, '\0');
                if (length) support.memory().read_bytes(entry + 21, std::span(reinterpret_cast<std::uint8_t*>(name.data()), name.size()));
                if (name.ends_with(".framework") || name.ends_with(".bundle") || name.ends_with(".plugin") || name.ends_with(".app") ||
                    name.ends_with(".xpc"))
                    names.push_back(name);
            }
            static_cast<void>(support.call(libsystem, "_closedir", {handle}));
            for (const auto& name : names)
                remember(support.send_id(class_named(support, "NSBundle"), "bundleWithPath:", {string(support, directory + "/" + name)}));
        }
    }
    // The image path containing `address` (dladdr), or empty.
    std::string image_of(Address address) {
        GuestBlock info(support, 32);
        if (!support.call(libsystem, "_dladdr", {address, info})) return {};
        return c_string(support.memory(), support.memory().read64(info), 4096);
    }
    std::optional<std::string> image_resource(Address bundle, Address name) {
        if (!name) return std::nullopt;
        const auto text_name = text(support, name);
        const auto dot = text_name.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            const auto found = support.send_id(bundle, "pathForResource:ofType:", {string(support, text_name.substr(0, dot)), string(support, text_name.substr(dot + 1))});
            if (found) return text(support, found);
        }
        for (const auto* type : {"png", "tiff", "tif", "jpg", "jpeg", "gif", "icns", "pdf", "bmp", "heic"}) {
            const auto found = support.send_id(bundle, "pathForResource:ofType:", {name, string(support, type)});
            if (found) return text(support, found);
        }
        return std::nullopt;
    }
};

} // namespace

void install_bundles(Support& support) {
    auto self = std::make_shared<Bundles>(support);
    const auto bundle = class_named(support, "NSBundle");
    support.method(bundle, "bundleWithURL:", [self](CpuState& s, GuestMemory&) {
        const auto path = s.x[2] ? self->support.cf.url_path(s.x[2]) : std::string{};
        s.x[0] = path.empty() ? 0 : self->support.send_id(s.x[0], "bundleWithPath:", {string(self->support, path)});
        self->remember(s.x[0]);
    }, "@@:@", true);
    support.method(bundle, "initWithURL:", [self](CpuState& s, GuestMemory&) {
        const auto path = s.x[2] ? self->support.cf.url_path(s.x[2]) : std::string{};
        s.x[0] = self->support.send_id(s.x[0], "initWithPath:", {path.empty() ? 0 : string(self->support, path)});
    }, "@@:@");
    support.method(bundle, "bundleWithIdentifier:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        self->scan();
        const auto key = text(self->support, s.x[2]);
        std::lock_guard lock(self->mutex);
        const auto found = self->by_identifier.find(key);
        s.x[0] = found == self->by_identifier.end() ? 0 : found->second;
    }, "@@:@", true);
    support.method(bundle, "classNamed:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto cls = self->support.call(foundation_library, "_NSClassFromString", {s.x[2]});
        if (!cls) { s.x[0] = 0; return; }
        const auto image = self->image_of(cls);
        const auto base = self->path_of(s.x[0]);
        s.x[0] = !image.empty() && image.starts_with(base + "/") ? cls : 0;
    }, "#@:@");
    support.method(bundle, "principalClass", [self](CpuState& s, GuestMemory&) {
        const auto name = self->support.send_id(s.x[0], "objectForInfoDictionaryKey:", {string(self->support, "NSPrincipalClass")});
        s.x[0] = name ? self->support.send_id(s.x[0], "classNamed:", {name}) : 0;
    }, "#@:");
    const auto auxiliary = [self](Address object, Address name) -> std::optional<std::string> {
        if (!name) return std::nullopt;
        const auto base = self->path_of(object);
        const auto contents = self->exists(base + "/Contents") ? base + "/Contents/MacOS/" : base + "/";
        const auto path = contents + text(self->support, name);
        return self->exists(path) ? std::optional<std::string>(path) : std::nullopt;
    };
    support.method(bundle, "pathForAuxiliaryExecutable:", [self, auxiliary](CpuState& s, GuestMemory&) {
        const auto path = auxiliary(s.x[0], s.x[2]);
        s.x[0] = path ? string(self->support, *path) : 0;
    }, "@@:@");
    support.method(bundle, "URLForAuxiliaryExecutable:", [self, auxiliary](CpuState& s, GuestMemory&) {
        const auto path = auxiliary(s.x[0], s.x[2]);
        s.x[0] = path ? autoreleased(self->support, self->support.cf.file_url(*path, false)) : 0;
    }, "@@:@");
    support.method(bundle, "pathForImageResource:", [self](CpuState& s, GuestMemory&) {
        const auto path = self->image_resource(s.x[0], s.x[2]);
        s.x[0] = path ? string(self->support, *path) : 0;
    }, "@@:@");
    support.method(bundle, "URLForImageResource:", [self](CpuState& s, GuestMemory&) {
        const auto path = self->image_resource(s.x[0], s.x[2]);
        s.x[0] = path ? autoreleased(self->support, self->support.cf.file_url(*path, false)) : 0;
    }, "@@:@");
    support.method(bundle, "imageForResource:", [self](CpuState& s, GuestMemory&) {
        const auto path = self->image_resource(s.x[0], s.x[2]);
        if (!path) { s.x[0] = 0; return; }
        const auto image_class = self->support.objc.find_class("NSImage");
        if (!image_class) unsupported("-[NSBundle imageForResource:] without NSImage");
        const auto image = self->support.send_id(self->support.send_id(image_class, "alloc"), "initWithContentsOfFile:", {string(self->support, *path)});
        s.x[0] = autoreleased(self->support, image);
    }, "@@:@");

    // CFDictionaryRef CFBundleCopyInfoDictionaryInDirectory(CFURLRef bundleURL)
    support.function(core_foundation, "_CFBundleCopyInfoDictionaryInDirectory", [self](CpuState& s, GuestMemory&) {
        const auto path = s.x[0] ? self->support.cf.url_path(s.x[0]) : std::string{};
        const auto object = path.empty() ? 0 : self->support.send_id(class_named(self->support, "NSBundle"), "bundleWithPath:", {string(self->support, path)});
        const auto info = object ? self->support.send_id(object, "infoDictionary") : 0;
        s.x[0] = info && self->support.send_id(info, "count") ? objc_retain(self->support, info) : 0;
    });
}

} // namespace alchemy::gui::completion_kit
