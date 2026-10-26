#include "alchemy/darwin/vision/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// Vision: no request can be performed (NO plus an NSError in VNErrorDomain);
// no barcode symbology is supported.
// String constants carry the values of Apple's public headers; values
// marked "(unverified)" follow the framework's naming convention and could
// not be checked against a header. Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::vision {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view vision = "/System/Library/Frameworks/Vision.framework/Versions/A/Vision";
constexpr std::int64_t vn_error_not_implemented = 8;

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(vision));
    const auto empty_array = [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); };
    for (const auto* name : {"VNBarcodeSymbologyAztec", "VNBarcodeSymbologyCode128", "VNBarcodeSymbologyCode39",
                             "VNBarcodeSymbologyCode39Checksum", "VNBarcodeSymbologyCode39FullASCII",
                             "VNBarcodeSymbologyCode39FullASCIIChecksum", "VNBarcodeSymbologyCode93",
                             "VNBarcodeSymbologyCode93i", "VNBarcodeSymbologyDataMatrix", "VNBarcodeSymbologyEAN13",
                             "VNBarcodeSymbologyEAN8", "VNBarcodeSymbologyI2of5", "VNBarcodeSymbologyI2of5Checksum",
                             "VNBarcodeSymbologyITF14", "VNBarcodeSymbologyPDF417", "VNBarcodeSymbologyQR",
                             "VNBarcodeSymbologyUPCE"})
        string(vision, std::string("_") + name, name);   // (unverified: value = name)
    string(vision, "_VNErrorDomain", "com.apple.Vision");
    const auto vn_request = cls(vision, "VNRequest", "NSObject");
    object_properties(vn_request, {"results"});
    integer_properties(vn_request, {"revision", "preferBackgroundProcessing", "usesCPUOnly"});
    const auto image_request = cls(vision, "VNImageBasedRequest", "VNRequest");
    static_cast<void>(image_request);
    const auto barcodes = cls(vision, "VNDetectBarcodesRequest", "VNImageBasedRequest");
    object_properties(barcodes, {"symbologies"});
    method(barcodes, "supportedSymbologiesAndReturnError:", [this](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        s.x[0] = bridge->autorelease(cf.array({}));
    });
    method(barcodes, "supportedSymbologies", empty_array, true);
    // Its constellation (VNRequestFaceLandmarksConstellation) and the faces to look at; performing
    // it fails like every request.
    const auto landmarks = cls(vision, "VNDetectFaceLandmarksRequest", "VNImageBasedRequest");
    integer_properties(landmarks, {"constellation"});
    object_properties(landmarks, {"inputFaceObservations"});
    const auto handler = cls(vision, "VNImageRequestHandler", "NSObject");
    object_properties(handler, {"image", "options"});
    for (const auto* selector : {"initWithCVPixelBuffer:options:", "initWithCGImage:options:", "initWithCIImage:options:",
                                 "initWithData:options:", "initWithURL:options:"})
        method(handler, selector, [this](CpuState& s, GuestMemory&) {
            store(s.x[0], "image", s.x[2]);
            store(s.x[0], "options", s.x[3]);
        });
    method(handler, "performRequests:error:", [this](CpuState& s, GuestMemory&) {
        bridge->store_error(s.x[3], bridge->error("com.apple.Vision", vn_error_not_implemented, "Vision requests are not available"), true);
        s.x[0] = 0;
    }, false, "B@:@^@");
}

} // namespace

void install_vision(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::vision
