#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// Metal: the descriptor and acceleration-structure classes, so programs that
// subclass or reference them bind (no device exists: darwin/shells.hpp).
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

constexpr std::string_view metal = darwin_framework_libraries[2];

class BaseClasses final : public Provider {
public:
    using Provider::Provider;
    void install() {
        constexpr std::pair<std::string_view, std::string_view> classes[] = {
            {"MTLAccelerationStructureGeometryDescriptor", "NSObject"},
            {"MTLAccelerationStructureBoundingBoxGeometryDescriptor", "MTLAccelerationStructureGeometryDescriptor"},
            {"MTLAccelerationStructureTriangleGeometryDescriptor", "MTLAccelerationStructureGeometryDescriptor"},
            {"MTLAccelerationStructureCurveGeometryDescriptor", "MTLAccelerationStructureGeometryDescriptor"},
            {"MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor", "MTLAccelerationStructureGeometryDescriptor"},
            {"MTLAccelerationStructureMotionCurveGeometryDescriptor", "MTLAccelerationStructureGeometryDescriptor"},
            {"MTLAccelerationStructureMotionTriangleGeometryDescriptor", "MTLAccelerationStructureGeometryDescriptor"},
            {"MTLMotionKeyframeData", "NSObject"},
            {"MTLAccelerationStructureDescriptor", "NSObject"},
            {"MTLInstanceAccelerationStructureDescriptor", "MTLAccelerationStructureDescriptor"},
            {"MTLPrimitiveAccelerationStructureDescriptor", "MTLAccelerationStructureDescriptor"},
            {"MTLRenderPassAttachmentDescriptor", "NSObject"},
            {"MTLRenderPassColorAttachmentDescriptor", "MTLRenderPassAttachmentDescriptor"},
            {"MTLFunctionDescriptor", "NSObject"}, {"MTLIntersectionFunctionDescriptor", "MTLFunctionDescriptor"},
            {"MTLArgumentDescriptor", "NSObject"}, {"MTLBinaryArchiveDescriptor", "NSObject"},
            {"MTLCaptureDescriptor", "NSObject"}, {"MTLCaptureManager", "NSObject"},
            {"MTLCommandBufferDescriptor", "NSObject"}, {"MTLCompileOptions", "NSObject"},
            {"MTLComputePassDescriptor", "NSObject"}, {"MTLComputePipelineDescriptor", "NSObject"},
            {"MTLCounterSampleBufferDescriptor", "NSObject"}, {"MTLDepthStencilDescriptor", "NSObject"},
            {"MTLFunctionConstantValues", "NSObject"}, {"MTLIndirectCommandBufferDescriptor", "NSObject"},
            {"MTLIntersectionFunctionTableDescriptor", "NSObject"}, {"MTLLinkedFunctions", "NSObject"},
            {"MTLRenderPassDescriptor", "NSObject"}, {"MTLRenderPipelineDescriptor", "NSObject"},
            {"MTLSamplerDescriptor", "NSObject"}, {"MTLSharedEventListener", "NSObject"},
            {"MTLStencilDescriptor", "NSObject"}, {"MTLTextureDescriptor", "NSObject"},
            {"MTLVertexDescriptor", "NSObject"}};
        for (const auto& [name, parent] : classes) static_cast<void>(declare_class(metal, name, parent));
    }
};

} // namespace

void install_metal_base_classes(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<BaseClasses>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
