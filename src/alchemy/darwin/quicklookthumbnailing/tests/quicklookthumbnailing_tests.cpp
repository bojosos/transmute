// QuickLookThumbnailing (src/alchemy/darwin/quicklookthumbnailing/): a PNG
// file's thumbnail at the request's size, a saved representation, and the
// generation-failed error for a file that is no image when icons are not
// allowed.
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/path.hpp"

#include <bit>
#include <filesystem>

namespace {
using namespace fill_test;

constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view image_io = "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO";

std::vector<std::uint64_t> answers;

Address global_block(Fixture& f, transmute::GuestCallHandler handler) {
    static int serial = 0;
    const auto invoke = f.process.install_function("_qlt_test_block_" + std::to_string(serial++), std::move(handler), "/usr/lib/libqlttest.dylib");
    auto isa = f.process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock");
    if (!isa) isa = f.process.resolve("/usr/lib/system/libsystem_blocks.dylib", "__NSConcreteGlobalBlock");
    require(isa.has_value(), "the global block class");
    const auto descriptor = f.block(16);
    f.engine.write64(descriptor + 8, 32);
    const auto literal = f.block(32);
    f.engine.write64(literal, isa->address);
    f.engine.write64(literal + 8, 1ULL << 28);   // BLOCK_IS_GLOBAL
    f.engine.write64(literal + 16, invoke);
    f.engine.write64(literal + 24, descriptor);
    return literal;
}

const Case quicklookthumbnailing_case("quicklookthumbnailing", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library("/System/Library/Frameworks/QuickLookThumbnailing.framework/Versions/A/QuickLookThumbnailing"),
            "QuickLookThumbnailing identity");
    // A 64 x 32 red PNG.
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlt_test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    const auto space = f.call(cg, "_CGColorSpaceCreateDeviceRGB");
    const auto context = f.call(cg, "_CGBitmapContextCreate", {0, 64, 32, 8, 0, space, 0x2002});
    alchemy::CallRegs color{};
    color.set_d(0, 1); color.set_d(1, 0); color.set_d(2, 0); color.set_d(3, 1);
    const auto red = f.regs(cg, "_CGColorCreateSRGB", color).x0;
    static_cast<void>(f.call(cg, "_CGContextSetFillColorWithColor", {context, red}));
    alchemy::CallRegs fill{};
    fill.x[0] = context;
    fill.set_d(0, 0); fill.set_d(1, 0); fill.set_d(2, 64); fill.set_d(3, 32);
    static_cast<void>(f.regs(cg, "_CGContextFillRect", fill));
    const auto picture = f.call(cg, "_CGBitmapContextCreateImage", {context});
    const auto png = f.cf.file_url(alchemy::guest_path_string(directory / "red.png"), false);
    const auto destination = f.call(image_io, "_CGImageDestinationCreateWithURL", {png, f.cf.string("public.png"), 1, 0});
    static_cast<void>(f.call(image_io, "_CGImageDestinationAddImage", {destination, picture, 0}));
    require((f.call(image_io, "_CGImageDestinationFinalize", {destination}) & 0xff) == 1, "the test PNG is written");

    // Its thumbnail at 16 x 16 points, scale 2: the long side is 32 pixels.
    const auto request_class = f.cls("QLThumbnailGenerationRequest");
    alchemy::CallRegs init{};
    init.x[2] = png;
    init.set_d(0, 16); init.set_d(1, 16); init.set_d(2, 2);
    init.x[3] = 4;   // QLThumbnailGenerationRequestRepresentationTypeThumbnail
    const auto request = f.objc.send(f.send(request_class, "alloc"), "initWithFileAtURL:size:scale:representationTypes:", init).x0;
    const auto generator = f.send(f.cls("QLThumbnailGenerator"), "sharedGenerator");
    require(generator && generator == f.send(f.cls("QLThumbnailGenerator"), "sharedGenerator"), "the shared generator");
    const auto handler = global_block(f, [](transmute::CpuState& s, transmute::GuestMemory&) { answers = {s.x[1], s.x[2]}; });
    answers.clear();
    static_cast<void>(f.send(generator, "generateBestRepresentationForRequest:completionHandler:", {request, handler}));
    require(answers.size() == 2 && answers[0] && !answers[1], "a representation and no error");
    const auto image = f.send(answers[0], "CGImage");
    require(f.call(cg, "_CGImageGetWidth", {image}) == 32 && f.call(cg, "_CGImageGetHeight", {image}) == 16, "the thumbnail keeps the aspect");
    require(f.send(answers[0], "type") == 2, "a thumbnail representation");

    // The best representation saved to a file.
    const auto saved = f.cf.file_url(alchemy::guest_path_string(directory / "thumb.png"), false);
    const auto save_handler = global_block(f, [](transmute::CpuState& s, transmute::GuestMemory&) { answers = {s.x[1]}; });
    answers.clear();
    static_cast<void>(f.send(generator, "saveBestRepresentationForRequest:toFileAtURL:withContentType:completionHandler:", {request, saved, 0, save_handler}));
    require(answers.size() == 1 && !answers[0] && std::filesystem::exists(directory / "thumb.png"), "the thumbnail is saved");

    // Not an image, icons not allowed: the generation fails.
    const auto text_url = f.cf.file_url(alchemy::guest_path_string(directory / "missing.txt"), false);
    init.x[2] = text_url;
    const auto failing = f.objc.send(f.send(request_class, "alloc"), "initWithFileAtURL:size:scale:representationTypes:", init).x0;
    answers.clear();
    static_cast<void>(f.send(generator, "generateBestRepresentationForRequest:completionHandler:", {failing, handler}));
    require(answers.size() == 2 && !answers[0] && answers[1], "an error and no representation");
    require(f.cf.utf8(f.send(answers[1], "domain")) == "QLThumbnailErrorDomain", "the error domain");
    std::filesystem::remove_all(directory, ignored);
});

} // namespace
