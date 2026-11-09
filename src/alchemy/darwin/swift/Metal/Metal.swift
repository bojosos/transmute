// Alchemy's project-authored Metal overlay (/usr/lib/swift/libswiftMetal.dylib):
// Apple's Swift extensions of the Metal protocols (array and Range forms of
// the C-array and NSRange calls), written from Apple's public documentation.
// Each forwards to the Objective-C method it stands for (AlchemyMetal.h),
// which Alchemy's Metal provider implements; nothing here binds a Metal
// symbol.

import Foundation
@_exported import Metal // The Clang module (Alchemy's Metal declarations).

@inline(__always)
internal func _range(_ range: Range<Int>) -> NSRange {
    return NSRange(location: range.lowerBound, length: range.count)
}

extension MTLBuffer {
    public func didModifyRange(_ range: Range<Int>) {
        __alchemyDidModify(_range(range))
    }

    public func addDebugMarker(_ marker: String, range: Range<Int>) {
        __alchemyAddDebugMarker(marker._bridgeToObjectiveC(), range: _range(range))
    }
}

extension MTLTexture {
    public func makeTextureView(pixelFormat: MTLPixelFormat, textureType: MTLTextureType, levels levelRange: Range<Int>,
                                slices sliceRange: Range<Int>) -> MTLTexture? {
        return __alchemyTextureView(pixelFormat: pixelFormat, textureType: textureType, levels: _range(levelRange),
                                    slices: _range(sliceRange))
    }
}

extension MTLBlitCommandEncoder {
    public func fill(buffer: MTLBuffer, range: Range<Int>, value: UInt8) {
        __alchemyFill(buffer: buffer, range: _range(range), value: value)
    }
}

extension MTLComputeCommandEncoder {
    public func useResources(_ resources: [MTLResource], usage: MTLResourceUsage) {
        __alchemyUseResources(resources, count: resources.count, usage: usage)
    }

    public func useHeaps(_ heaps: [MTLHeap]) {
        __alchemyUseHeaps(heaps, count: heaps.count)
    }

    public func setBuffers(_ buffers: [MTLBuffer?], offsets: [Int], range: Range<Int>) {
        precondition(buffers.count >= range.count && offsets.count >= range.count, "fewer buffers or offsets than the range")
        __alchemySetBuffers(buffers, offsets: offsets, range: _range(range))
    }

    public func setTextures(_ textures: [MTLTexture?], range: Range<Int>) {
        precondition(textures.count >= range.count, "fewer textures than the range")
        __alchemySetTextures(textures, range: _range(range))
    }

    public func setSamplerStates(_ samplers: [MTLSamplerState?], range: Range<Int>) {
        precondition(samplers.count >= range.count, "fewer samplers than the range")
        __alchemySetSamplerStates(samplers, range: _range(range))
    }

    public func setSamplerStates(_ samplers: [MTLSamplerState?], lodMinClamps: [Float], lodMaxClamps: [Float],
                                 range: Range<Int>) {
        precondition(samplers.count >= range.count && lodMinClamps.count >= range.count && lodMaxClamps.count >= range.count,
                     "fewer samplers or clamps than the range")
        __alchemySetSamplerStates(samplers, lodMinClamps: lodMinClamps, lodMaxClamps: lodMaxClamps, range: _range(range))
    }

    public func memoryBarrier(resources: [MTLResource]) {
        __alchemyMemoryBarrier(resources: resources, count: resources.count)
    }
}

extension MTLRenderCommandEncoder {
    public func useResources(_ resources: [MTLResource], usage: MTLResourceUsage) {
        __alchemyUseResources(resources, count: resources.count, usage: usage)
    }

    public func useResources(_ resources: [MTLResource], usage: MTLResourceUsage, stages: MTLRenderStages) {
        __alchemyUseResources(resources, count: resources.count, usage: usage, stages: stages)
    }

    public func useHeaps(_ heaps: [MTLHeap]) {
        __alchemyUseHeaps(heaps, count: heaps.count)
    }

    public func useHeaps(_ heaps: [MTLHeap], stages: MTLRenderStages) {
        __alchemyUseHeaps(heaps, count: heaps.count, stages: stages)
    }

    public func setViewports(_ viewports: [MTLViewport]) {
        __alchemySetViewports(viewports, count: viewports.count)
    }

    public func setScissorRects(_ scissorRects: [MTLScissorRect]) {
        __alchemySetScissorRects(scissorRects, count: scissorRects.count)
    }

    public func setVertexBuffers(_ buffers: [MTLBuffer?], offsets: [Int], range: Range<Int>) {
        precondition(buffers.count >= range.count && offsets.count >= range.count, "fewer buffers or offsets than the range")
        __alchemySetVertexBuffers(buffers, offsets: offsets, range: _range(range))
    }

    public func setVertexTextures(_ textures: [MTLTexture?], range: Range<Int>) {
        precondition(textures.count >= range.count, "fewer textures than the range")
        __alchemySetVertexTextures(textures, range: _range(range))
    }

    public func setVertexSamplerStates(_ samplers: [MTLSamplerState?], range: Range<Int>) {
        precondition(samplers.count >= range.count, "fewer samplers than the range")
        __alchemySetVertexSamplerStates(samplers, range: _range(range))
    }

    public func setVertexSamplerStates(_ samplers: [MTLSamplerState?], lodMinClamps: [Float], lodMaxClamps: [Float],
                                       range: Range<Int>) {
        precondition(samplers.count >= range.count && lodMinClamps.count >= range.count && lodMaxClamps.count >= range.count,
                     "fewer samplers or clamps than the range")
        __alchemySetVertexSamplerStates(samplers, lodMinClamps: lodMinClamps, lodMaxClamps: lodMaxClamps, range: _range(range))
    }

    public func setFragmentBuffers(_ buffers: [MTLBuffer?], offsets: [Int], range: Range<Int>) {
        precondition(buffers.count >= range.count && offsets.count >= range.count, "fewer buffers or offsets than the range")
        __alchemySetFragmentBuffers(buffers, offsets: offsets, range: _range(range))
    }

    public func setFragmentTextures(_ textures: [MTLTexture?], range: Range<Int>) {
        precondition(textures.count >= range.count, "fewer textures than the range")
        __alchemySetFragmentTextures(textures, range: _range(range))
    }

    public func setFragmentSamplerStates(_ samplers: [MTLSamplerState?], range: Range<Int>) {
        precondition(samplers.count >= range.count, "fewer samplers than the range")
        __alchemySetFragmentSamplerStates(samplers, range: _range(range))
    }

    public func setFragmentSamplerStates(_ samplers: [MTLSamplerState?], lodMinClamps: [Float], lodMaxClamps: [Float],
                                         range: Range<Int>) {
        precondition(samplers.count >= range.count && lodMinClamps.count >= range.count && lodMaxClamps.count >= range.count,
                     "fewer samplers or clamps than the range")
        __alchemySetFragmentSamplerStates(samplers, lodMinClamps: lodMinClamps, lodMaxClamps: lodMaxClamps, range: _range(range))
    }

    public func memoryBarrier(resources: [MTLResource], after: MTLRenderStages, before: MTLRenderStages) {
        __alchemyMemoryBarrier(resources: resources, count: resources.count, after: after, before: before)
    }
}

extension MTLArgumentEncoder {
    public func setBuffers(_ buffers: [MTLBuffer?], offsets: [Int], range: Range<Int>) {
        precondition(buffers.count >= range.count && offsets.count >= range.count, "fewer buffers or offsets than the range")
        __alchemySetBuffers(buffers, offsets: offsets, range: _range(range))
    }

    public func setTextures(_ textures: [MTLTexture?], range: Range<Int>) {
        precondition(textures.count >= range.count, "fewer textures than the range")
        __alchemySetTextures(textures, range: _range(range))
    }

    public func setSamplerStates(_ samplers: [MTLSamplerState?], range: Range<Int>) {
        precondition(samplers.count >= range.count, "fewer samplers than the range")
        __alchemySetSamplerStates(samplers, range: _range(range))
    }

    public func setRenderPipelineStates(_ pipelines: [MTLRenderPipelineState?], range: Range<Int>) {
        precondition(pipelines.count >= range.count, "fewer pipeline states than the range")
        __alchemySetRenderPipelineStates(pipelines, range: _range(range))
    }

    public func setComputePipelineStates(_ pipelines: [MTLComputePipelineState?], range: Range<Int>) {
        precondition(pipelines.count >= range.count, "fewer pipeline states than the range")
        __alchemySetComputePipelineStates(pipelines, range: _range(range))
    }

    public func setIndirectCommandBuffers(_ buffers: [MTLIndirectCommandBuffer?], range: Range<Int>) {
        precondition(buffers.count >= range.count, "fewer indirect command buffers than the range")
        __alchemySetIndirectCommandBuffers(buffers, range: _range(range))
    }
}

extension MTLIndirectCommandBuffer {
    public func reset(_ range: Range<Int>) {
        __alchemyReset(_range(range))
    }
}

extension MTLFunctionConstantValues {
    public func setConstantValues(_ values: UnsafeRawPointer, type: MTLDataType, range: Range<Int>) {
        __alchemySetConstantValues(values, type: type, range: _range(range))
    }
}

extension MTLDevice {
    public func getDefaultSamplePositions(sampleCount: Int) -> [MTLSamplePosition] {
        var positions = [MTLSamplePosition](repeating: MTLSamplePosition(x: 0, y: 0), count: sampleCount)
        positions.withUnsafeMutableBufferPointer { buffer in
            if let base = buffer.baseAddress { __alchemyGetDefaultSamplePositions(base, count: sampleCount) }
        }
        return positions
    }
}
