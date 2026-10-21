// Alchemy's project-authored CoreMIDI overlay (/usr/lib/swift/
// libswiftCoreMIDI.dylib): Apple's Swift sequence over the packets of a
// MIDIPacketList, written from Apple's public documentation. Pure Swift over
// the C layout: MIDIPacket and MIDIPacketList are packed to 4 bytes (the
// packet data starts at byte 10, the first packet at byte 4 of the list), and
// on arm64 the next packet starts at the first 4-byte boundary after the
// previous packet's data, as MIDIPacketNext documents.

@_exported import CoreMIDI // The Clang module (Alchemy's CoreMIDI declarations).

// Byte offsets of the packed layout.
internal let _packetDataOffset = MemoryLayout<MIDITimeStamp>.size + MemoryLayout<UInt16>.size
internal let _firstPacketOffset = MemoryLayout<UInt32>.size

// MIDIPacketNext on arm64.
internal func _nextPacket(_ packet: UnsafePointer<MIDIPacket>) -> UnsafePointer<MIDIPacket> {
    let dataEnd = UInt(bitPattern: UnsafeRawPointer(packet) + _packetDataOffset + Int(packet.pointee.length))
    let next = (dataEnd + 3) & ~UInt(3)
    return UnsafeRawPointer(bitPattern: next)!.assumingMemoryBound(to: MIDIPacket.self)
}

extension MIDIPacketList {
    /// A sequence of pointers to the packets of a packet list, valid while the list's memory is.
    public struct UnsafeSequence: Sequence {
        public typealias Element = UnsafePointer<MIDIPacket>

        /// The packet list.
        internal let unsafePointer: UnsafePointer<MIDIPacketList>

        /// Creates a sequence over the packets of the given list.
        public init(_ list: UnsafePointer<MIDIPacketList>) {
            unsafePointer = list
        }

        /// An iterator over the packets of a packet list.
        public struct Iterator: IteratorProtocol {
            internal var packet: UnsafePointer<MIDIPacket>
            internal var remaining: Int

            internal init(_ list: UnsafePointer<MIDIPacketList>) {
                packet = (UnsafeRawPointer(list) + _firstPacketOffset).assumingMemoryBound(to: MIDIPacket.self)
                remaining = Int(list.pointee.numPackets)
            }

            /// The next packet, or nil after the last one.
            public mutating func next() -> UnsafePointer<MIDIPacket>? {
                guard remaining > 0 else { return nil }
                let current = packet
                remaining -= 1
                if remaining > 0 { packet = _nextPacket(current) }
                return current
            }
        }

        public func makeIterator() -> Iterator { return Iterator(unsafePointer) }
        public var underestimatedCount: Int { return Int(unsafePointer.pointee.numPackets) }
    }
}

extension UnsafePointer where Pointee == MIDIPacketList {
    /// The packets of the list as a sequence of packet pointers.
    public func unsafeSequence() -> MIDIPacketList.UnsafeSequence {
        return MIDIPacketList.UnsafeSequence(self)
    }
}

extension UnsafeMutablePointer where Pointee == MIDIPacketList {
    /// The packets of the list as a sequence of packet pointers.
    public func unsafeSequence() -> MIDIPacketList.UnsafeSequence {
        return MIDIPacketList.UnsafeSequence(UnsafePointer(self))
    }
}
