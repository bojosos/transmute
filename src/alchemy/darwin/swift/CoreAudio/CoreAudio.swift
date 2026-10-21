// Alchemy's project-authored CoreAudio overlay (/usr/lib/swift/
// libswiftCoreAudio.dylib): Apple's Swift API over AudioBuffer and
// AudioBufferList, written from Apple's public documentation. Pure Swift
// over the C layouts; the only C function is libSystem's calloc.

@_exported import CoreAudio // The Clang module (Alchemy's CoreAudio declarations).

extension UnsafeBufferPointer {
    /// Creates a buffer pointer over the audio buffer's data, interpreted as `Element` values.
    public init(_ audioBuffer: AudioBuffer) {
        self.init(start: audioBuffer.mData?.assumingMemoryBound(to: Element.self),
                  count: Int(audioBuffer.mDataByteSize) / MemoryLayout<Element>.stride)
    }
}

extension UnsafeMutableBufferPointer {
    /// Creates a mutable buffer pointer over the audio buffer's data, interpreted as `Element` values.
    public init(_ audioBuffer: AudioBuffer) {
        self.init(start: audioBuffer.mData?.assumingMemoryBound(to: Element.self),
                  count: Int(audioBuffer.mDataByteSize) / MemoryLayout<Element>.stride)
    }
}

extension AudioBuffer {
    /// Creates an audio buffer over the given typed buffer with the given number of interleaved channels.
    public init<Element>(_ typedBuffer: UnsafeMutableBufferPointer<Element>, numberOfChannels: Int) {
        self.init(mNumberChannels: UInt32(numberOfChannels),
                  mDataByteSize: UInt32(typedBuffer.count * MemoryLayout<Element>.stride),
                  mData: UnsafeMutableRawPointer(typedBuffer.baseAddress))
    }
}

extension AudioBufferList {
    /// The size in bytes of an audio buffer list that holds up to `maximumBuffers` buffers.
    public static func sizeInBytes(maximumBuffers: Int) -> Int {
        precondition(maximumBuffers >= 1, "AudioBufferList should contain at least one AudioBuffer")
        return MemoryLayout<AudioBufferList>.size - MemoryLayout<AudioBuffer>.size + maximumBuffers * MemoryLayout<AudioBuffer>.size
    }

    /// Allocates a zeroed audio buffer list for `maximumBuffers` buffers, with
    /// mNumberBuffers set to `maximumBuffers`. Release the memory with free().
    public static func allocate(maximumBuffers: Int) -> UnsafeMutableAudioBufferListPointer {
        let byteSize = sizeInBytes(maximumBuffers: maximumBuffers)
        guard let memory = __calloc(byteSize, 1) else {
            fatalError("AudioBufferList.allocate: out of memory (\(byteSize) bytes)")
        }
        let list = memory.bindMemory(to: AudioBufferList.self, capacity: 1)
        list.pointee.mNumberBuffers = UInt32(maximumBuffers)
        return UnsafeMutableAudioBufferListPointer(list)
    }
}

/// A wrapper for a pointer to an audio buffer list that accesses its variable-length array of buffers as a collection.
public struct UnsafeMutableAudioBufferListPointer {
    /// The underlying pointer to the audio buffer list.
    public var unsafeMutablePointer: UnsafeMutablePointer<AudioBufferList>

    /// Creates a wrapper for the given audio buffer list pointer.
    public init(_ p: UnsafeMutablePointer<AudioBufferList>) {
        unsafeMutablePointer = p
    }

    /// Creates a wrapper for the given audio buffer list pointer, or nil for a nil pointer.
    public init?(_ p: UnsafeMutablePointer<AudioBufferList>?) {
        guard let p = p else { return nil }
        self.init(p)
    }

    /// The number of buffers in the list (mNumberBuffers).
    public var count: Int {
        get { return Int(unsafeMutablePointer.pointee.mNumberBuffers) }
        nonmutating set(newValue) { unsafeMutablePointer.pointee.mNumberBuffers = UInt32(newValue) }
    }

    /// The underlying pointer, as an immutable pointer.
    public var unsafePointer: UnsafePointer<AudioBufferList> {
        return UnsafePointer(unsafeMutablePointer)
    }

    // The first element of the variable-length mBuffers array: mBuffers is
    // the list's last member, so it starts one AudioBuffer before the end of
    // a one-buffer list.
    internal var _audioBuffersPointer: UnsafeMutablePointer<AudioBuffer> {
        let end = UnsafeMutableRawPointer(unsafeMutablePointer + 1)
        return end.assumingMemoryBound(to: AudioBuffer.self) - 1
    }
}

extension UnsafeMutableAudioBufferListPointer: MutableCollection, RandomAccessCollection {
    public typealias Index = Int

    /// Always zero, the index of the first buffer.
    public var startIndex: Int { return 0 }

    /// The number of buffers, the index after the last buffer.
    public var endIndex: Int { return count }

    /// Accesses the buffer at the given index.
    public subscript(index: Index) -> AudioBuffer {
        get {
            precondition(index >= 0 && index < count, "subscript index out of range")
            return (_audioBuffersPointer + index).pointee
        }
        nonmutating set(newValue) {
            precondition(index >= 0 && index < count, "subscript index out of range")
            (_audioBuffersPointer + index).pointee = newValue
        }
    }
}
