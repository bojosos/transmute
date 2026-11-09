// CMSampleBuffer's Swift accessors (project-authored, from Apple's public
// CoreMedia documentation), over the CoreMedia provider's C functions.

extension CMSampleBuffer {
    /// The image buffer the sample buffer carries, or nil.
    public var imageBuffer: CVImageBuffer? {
        return CMSampleBufferGetImageBuffer(self)?.takeUnretainedValue()
    }

    /// The earliest presentation timestamp of the buffer's samples (invalid on error).
    public var presentationTimeStamp: CMTime {
        return CMSampleBufferGetPresentationTimeStamp(self)
    }
}
