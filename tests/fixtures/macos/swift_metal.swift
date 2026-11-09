// swift_metal: the project-authored Metal overlay (libswiftMetal: array and
// Range forms of the Metal calls) on Alchemy's Metal provider.
import Metal

@main
struct SwiftMetal {
    static func main() {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(),
              let a = device.makeBuffer(length: 64, options: []), let b = device.makeBuffer(length: 64, options: []),
              let commands = queue.makeCommandBuffer() else {
            print("swift_metal: no Metal device")
            return
        }
        let blit = commands.makeBlitCommandEncoder()!
        blit.fill(buffer: a, range: 0..<32, value: 7)
        blit.fill(buffer: a, range: 32..<64, value: 9)
        blit.endEncoding()
        let compute = commands.makeComputeCommandEncoder()!
        compute.setBuffers([a, b], offsets: [0, 16], range: 0..<2)
        compute.useResources([a, b], usage: [.read, .write])
        compute.memoryBarrier(resources: [a])
        compute.endEncoding()
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 8, height: 8, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead, .pixelFormatView]
        let target = device.makeTexture(descriptor: descriptor)!
        let pass = MTLRenderPassDescriptor.renderPassDescriptor()
        let color = pass.colorAttachments[0]
        color.texture = target
        color.loadAction = .clear
        color.storeAction = .store
        let render = commands.makeRenderCommandEncoder(descriptor: pass)!
        render.setViewports([MTLViewport(originX: 0, originY: 0, width: 8, height: 8, znear: 0, zfar: 1)])
        render.setScissorRects([MTLScissorRect(x: 0, y: 0, width: 8, height: 8)])
        render.setVertexBuffers([a, nil, b], offsets: [0, 0, 32], range: 0..<3)
        render.setFragmentBuffers([b], offsets: [0], range: 1..<2)
        render.useResources([a], usage: .read, stages: [.vertex, .fragment])
        render.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
        let bytes = a.contents().assumingMemoryBound(to: UInt8.self)
        print("swift_metal: fill \(bytes[0]) \(bytes[31]) \(bytes[32]) \(bytes[63])")
        let view = target.makeTextureView(pixelFormat: .bgra8Unorm, textureType: .type2D, levels: 0..<1, slices: 0..<1)
        print("swift_metal: view \(view != nil) \(view?.pixelFormat == .bgra8Unorm)")
        print("swift_metal: positions \(device.getDefaultSamplePositions(sampleCount: 4).count)")
        a.didModifyRange(0..<4)
        print("swift_metal: done")
    }
}