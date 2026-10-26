#include "alchemy/darwin/quicklookthumbnailing/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <algorithm>
#include <cmath>
#include <string>

// QuickLookThumbnailing: thumbnails of files. An image file's thumbnail is
// made by ImageIO (CGImageSourceCreateThumbnailAtIndex, scaled to the
// request's size); any other file answers its icon (NSWorkspace) when the
// request allows icons, and fails with QLThumbnailErrorGenerationFailed
// otherwise. Requests complete before the generating call returns (the
// handlers run on the calling thread). VS Code's Electron framework links it.
namespace alchemy::quicklookthumbnailing {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view library = "/System/Library/Frameworks/QuickLookThumbnailing.framework/Versions/A/QuickLookThumbnailing";
constexpr std::string_view image_io = "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO";
constexpr std::string_view core_graphics = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view error_domain = "QLThumbnailErrorDomain";
// QLThumbnailGenerationRequestRepresentationTypes and QLThumbnailRepresentationType.
constexpr std::uint64_t request_icon = 1, request_low_quality = 2, request_thumbnail = 4;
constexpr std::int64_t type_icon = 0, type_thumbnail = 2;
constexpr std::int64_t generation_failed = 0;   // QLThumbnailErrorGenerationFailed (unverified)

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    Address representation_class{};
    void install();
    double real(Address object, std::string_view key) {
        const auto values = properties(object);
        std::lock_guard lock(values->mutex);
        const auto found = values->reals.find(key);
        return found == values->reals.end() ? 0.0 : found->second;
    }
    void set_real(Address object, std::string_view key, double value) {
        const auto values = properties(object);
        std::lock_guard lock(values->mutex);
        values->reals[std::string(key)] = value;
    }
    Address number(double value) {
        CallRegs registers{};
        registers.set_d(0, value);
        return objc.send(objc.find_class("NSNumber"), "numberWithDouble:", registers).x0;
    }
    Address constant(std::string_view owner, std::string_view symbol) {
        const auto found = process.resolve(owner, symbol);
        return found ? memory().read64(found->address) : 0;
    }
    // A representation (autoreleased) of `request`, or 0.
    Address represent(Address request) {
        const auto url = stored(request, "fileURL");
        const auto types = static_cast<std::uint64_t>(stored_integer(request, "representationTypes"));
        const double scale = std::max(1.0, real(request, "scale"));
        const double longest = std::max(real(request, "width"), real(request, "height")) * scale;
        if (!url || !(longest > 0)) return 0;
        Address image{};
        std::int64_t type{};
        if (types & (request_thumbnail | request_low_quality)) {
            if (const auto source = call(image_io, "_CGImageSourceCreateWithURL", {url, 0})) {
                if (call(image_io, "_CGImageSourceGetCount", {source}) > 0) {
                    const auto yes = constant("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_kCFBooleanTrue");
                    const std::vector<std::pair<Address, Address>> entries{
                        {constant(image_io, "_kCGImageSourceCreateThumbnailFromImageAlways"), yes},
                        {constant(image_io, "_kCGImageSourceCreateThumbnailWithTransform"), yes},
                        {constant(image_io, "_kCGImageSourceThumbnailMaxPixelSize"), number(std::ceil(longest))}};
                    const auto options = cf.dictionary(entries);
                    image = call(image_io, "_CGImageSourceCreateThumbnailAtIndex", {source, 0, options});
                    release(options);
                    type = type_thumbnail;
                }
                release(source);
            }
        }
        if (!image && (types & request_icon)) {
            const auto workspace = objc.send(objc.find_class("NSWorkspace"), "sharedWorkspace").x0;
            CallRegs path_args{};
            const auto path = objc.send(url, "path").x0;
            path_args.x[2] = path;
            const auto icon = workspace && path ? objc.send(workspace, "iconForFile:", path_args).x0 : 0;
            if (icon) {
                CallRegs size{};
                size.set_d(0, longest / scale);
                size.set_d(1, longest / scale);
                static_cast<void>(objc.send(icon, "setSize:", size));
                CallRegs proposed{};
                if (const auto made = objc.send(icon, "CGImageForProposedRect:context:hints:", proposed).x0) image = retain(made);
                type = type_icon;
            }
        }
        if (!image) return 0;
        const auto value = create(representation_class);
        store(value, "CGImage", image);
        release(image);
        store_integer(value, "type", type);
        set_real(value, "scale", scale);
        return bridge->autorelease(value);
    }
    Address failure(Address request) {
        const auto url = stored(request, "fileURL");
        const auto path = url ? objc.send(url, "path").x0 : 0;
        return bridge->error(error_domain, generation_failed,
                             "No thumbnail could be made for " + (path ? cf.utf8(path) : std::string("the request")));
    }
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(library));
    string(library, "_QLThumbnailErrorDomain", error_domain);

    // QLThumbnailGenerationRequest.
    const auto request = cls(library, "QLThumbnailGenerationRequest", "NSObject");
    cleanup_on_dealloc(request);
    object_properties(request, {"contentType"});
    integer_properties(request, {"iconMode", "representationTypes"});
    real_properties(request, {"minimumDimension"});
    method(request, "initWithFileAtURL:size:scale:representationTypes:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "fileURL", s.x[2]);
        set_real(s.x[0], "width", std::bit_cast<double>(s.v[0][0]));
        set_real(s.x[0], "height", std::bit_cast<double>(s.v[1][0]));
        set_real(s.x[0], "scale", std::bit_cast<double>(s.v[2][0]));
        store_integer(s.x[0], "representationTypes", static_cast<std::int64_t>(s.x[3]));
    }, false, "@@:@{CGSize=dd}dQ");
    method(request, "size", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        s.v[0] = {std::bit_cast<std::uint64_t>(real(self, "width")), 0};
        s.v[1] = {std::bit_cast<std::uint64_t>(real(self, "height")), 0};
    }, false, "{CGSize=dd}@:");
    method(request, "scale", [this](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(real(s.x[0], "scale")), 0}; }, false, "d@:");

    // QLThumbnailRepresentation.
    representation_class = cls(library, "QLThumbnailRepresentation", "NSObject");
    cleanup_on_dealloc(representation_class);
    method(representation_class, "type", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(stored_integer(s.x[0], "type")); }, false, "q@:");
    method(representation_class, "CGImage", [this](CpuState& s, GuestMemory&) { s.x[0] = stored(s.x[0], "CGImage"); }, false, "^{CGImage=}@:");
    // The image's size in points (pixels / scale) from its origin.
    const auto points = [this](Address self) {
        const auto image = stored(self, "CGImage");
        const double scale = std::max(1.0, real(self, "scale"));
        return std::pair{call(core_graphics, "_CGImageGetWidth", {image}) / scale, call(core_graphics, "_CGImageGetHeight", {image}) / scale};
    };
    method(representation_class, "contentRect", [points](CpuState& s, GuestMemory&) {
        const auto [w, h] = points(s.x[0]);
        s.v[0] = {0, 0}; s.v[1] = {0, 0};
        s.v[2] = {std::bit_cast<std::uint64_t>(w), 0};
        s.v[3] = {std::bit_cast<std::uint64_t>(h), 0};
    }, false, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(representation_class, "NSImage", [this, points](CpuState& s, GuestMemory&) {
        const auto [w, h] = points(s.x[0]);
        CallRegs init{};
        init.x[2] = stored(s.x[0], "CGImage");
        init.set_d(0, w);
        init.set_d(1, h);
        const auto made = objc.send(objc.send(objc.find_class("NSImage"), "alloc").x0, "initWithCGImage:size:", init).x0;
        s.x[0] = made ? bridge->autorelease(made) : 0;
    }, false, "@@:");

    // QLThumbnailGenerator.
    const auto generator = cls(library, "QLThumbnailGenerator", "NSObject");
    singleton(generator, "sharedGenerator");
    method(generator, "generateBestRepresentationForRequest:completionHandler:", [this](CpuState& s, GuestMemory&) {
        const auto rep = represent(s.x[2]);
        if (!s.x[3]) return;
        if (rep) static_cast<void>(bridge->invoke_block(s.x[3], {rep, 0}));
        else static_cast<void>(bridge->invoke_block(s.x[3], {0, failure(s.x[2])}));
    }, false, "v@:@@?");
    method(generator, "generateRepresentationsForRequest:updateHandler:", [this](CpuState& s, GuestMemory&) {
        const auto rep = represent(s.x[2]);
        if (!s.x[3]) return;
        if (rep) static_cast<void>(bridge->invoke_block(s.x[3], {rep, static_cast<Address>(stored_integer(rep, "type")), 0}));
        else static_cast<void>(bridge->invoke_block(s.x[3], {0, static_cast<Address>(type_thumbnail), failure(s.x[2])}));
    }, false, "v@:@@?");
    // The best representation written to a PNG or JPEG file (by the content type's identifier).
    method(generator, "saveBestRepresentationForRequest:toFileAtURL:withContentType:completionHandler:", [this](CpuState& s, GuestMemory&) {
        const auto rep = represent(s.x[2]);
        Address error{};
        if (!rep) {
            error = failure(s.x[2]);
        } else {
            const auto identifier = s.x[4] ? objc.send(s.x[4], "identifier").x0 : 0;
            const auto type = identifier ? identifier : bridge->autorelease(cf.string("public.png"));
            const auto destination = call(image_io, "_CGImageDestinationCreateWithURL", {s.x[3], type, 1, 0});
            bool written{};
            if (destination) {
                static_cast<void>(call(image_io, "_CGImageDestinationAddImage", {destination, stored(rep, "CGImage"), 0}));
                written = (call(image_io, "_CGImageDestinationFinalize", {destination}) & 0xff) != 0;
                release(destination);
            }
            if (!written) error = bridge->error(error_domain, 1, "The thumbnail could not be saved");   // QLThumbnailErrorSavingToURLFailed (unverified)
        }
        if (s.x[5]) static_cast<void>(bridge->invoke_block(s.x[5], {error}));
    }, false, "v@:@@@@?");
    // Requests complete before the generating call returns: nothing is left to cancel.
    method(generator, "cancelRequest:", [](CpuState&, GuestMemory&) {}, false, "v@:@");
}

} // namespace

void install_quicklookthumbnailing(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::quicklookthumbnailing
