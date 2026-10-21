// HashFunction, Digest and the SHA-2 hash functions (SHA256, SHA384,
// SHA512). Input is buffered in the value and hashed by the host when the
// digest is finalized.

import Foundation

/// A type that represents the output of a hash.
public protocol Digest: Hashable, ContiguousBytes, CustomStringConvertible, Sequence where Element == UInt8 {
    /// The number of bytes in the digest.
    static var byteCount: Int { get }
}

/// A type that performs cryptographically secure hashing.
public protocol HashFunction {
    /// The number of bytes that represents the hash function's internal state.
    static var blockByteCount: Int { get }
    /// The type of the digest returned by the hash function.
    associatedtype Digest: CryptoKit.Digest
    /// Creates a hash function.
    init()
    /// Incrementally updates the hash function with the contents of the buffer.
    mutating func update(bufferPointer: UnsafeRawBufferPointer)
    /// Finalizes the hash function and returns the computed digest.
    func finalize() -> Digest
}

extension HashFunction {
    /// Incrementally updates the hash function with the given data.
    public mutating func update<D: DataProtocol>(data: D) {
        let bytes = _bytes(data: data)
        bytes.withUnsafeBytes { update(bufferPointer: $0) }
    }

    /// Computes a digest of the data.
    public static func hash<D: DataProtocol>(data: D) -> Digest {
        var hasher = Self()
        hasher.update(data: data)
        return hasher.finalize()
    }

    /// Computes a digest of the buffer.
    public static func hash(bufferPointer: UnsafeRawBufferPointer) -> Digest {
        var hasher = Self()
        hasher.update(bufferPointer: bufferPointer)
        return hasher.finalize()
    }
}

// The host algorithm number of a CryptoKit hash function, or nil for a
// hash function defined outside this module.
internal func _hostAlgorithm<H: HashFunction>(_ hash: H.Type) -> Int? {
    if hash == SHA256.self { return 1 }
    if hash == SHA384.self { return 2 }
    if hash == SHA512.self { return 3 }
    return nil
}

internal func _hostDigest(_ algorithm: Int, _ bytes: [UInt8], _ count: Int) -> [UInt8] {
    var digest = [UInt8](repeating: 0, count: count)
    let status = bytes.withUnsafeBytes { input in
        digest.withUnsafeMutableBytes { output in
            _alchemyHash(algorithm, input.baseAddress, input.count, output.baseAddress)
        }
    }
    precondition(status == _statusOK, "CryptoKit: hashing failed")
    return digest
}

// ---- SHA256 --------------------------------------------------------------------

/// The output of a Secure Hashing Algorithm 2 (SHA-2) hash with a 256-bit digest.
public struct SHA256Digest: Digest {
    internal let bytes: [UInt8]
    internal init(bytes: [UInt8]) { self.bytes = bytes }
    public static var byteCount: Int { return 32 }
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try bytes.withUnsafeBytes(body)
    }
    public func makeIterator() -> Array<UInt8>.Iterator { return bytes.makeIterator() }
    public var description: String { return "SHA256 digest: " + _hex(bytes) }
    public static func == (lhs: SHA256Digest, rhs: SHA256Digest) -> Bool { return _constantTimeEqual(lhs.bytes, rhs.bytes) }
    public func hash(into hasher: inout Hasher) { hasher.combine(bytes) }
}

/// An implementation of Secure Hashing Algorithm 2 (SHA-2) hashing with a 256-bit digest.
public struct SHA256: HashFunction {
    public typealias Digest = SHA256Digest
    internal var pending: [UInt8] = []
    public static var byteCount: Int { return 32 }
    public static var blockByteCount: Int { return 64 }
    public init() {}
    public mutating func update(bufferPointer: UnsafeRawBufferPointer) { pending.append(contentsOf: bufferPointer) }
    public func finalize() -> SHA256Digest { return SHA256Digest(bytes: _hostDigest(1, pending, 32)) }
}

// ---- SHA384 --------------------------------------------------------------------

/// The output of a Secure Hashing Algorithm 2 (SHA-2) hash with a 384-bit digest.
public struct SHA384Digest: Digest {
    internal let bytes: [UInt8]
    internal init(bytes: [UInt8]) { self.bytes = bytes }
    public static var byteCount: Int { return 48 }
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try bytes.withUnsafeBytes(body)
    }
    public func makeIterator() -> Array<UInt8>.Iterator { return bytes.makeIterator() }
    public var description: String { return "SHA384 digest: " + _hex(bytes) }
    public static func == (lhs: SHA384Digest, rhs: SHA384Digest) -> Bool { return _constantTimeEqual(lhs.bytes, rhs.bytes) }
    public func hash(into hasher: inout Hasher) { hasher.combine(bytes) }
}

/// An implementation of Secure Hashing Algorithm 2 (SHA-2) hashing with a 384-bit digest.
public struct SHA384: HashFunction {
    public typealias Digest = SHA384Digest
    internal var pending: [UInt8] = []
    public static var byteCount: Int { return 48 }
    public static var blockByteCount: Int { return 128 }
    public init() {}
    public mutating func update(bufferPointer: UnsafeRawBufferPointer) { pending.append(contentsOf: bufferPointer) }
    public func finalize() -> SHA384Digest { return SHA384Digest(bytes: _hostDigest(2, pending, 48)) }
}

// ---- SHA512 --------------------------------------------------------------------

/// The output of a Secure Hashing Algorithm 2 (SHA-2) hash with a 512-bit digest.
public struct SHA512Digest: Digest {
    internal let bytes: [UInt8]
    internal init(bytes: [UInt8]) { self.bytes = bytes }
    public static var byteCount: Int { return 64 }
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try bytes.withUnsafeBytes(body)
    }
    public func makeIterator() -> Array<UInt8>.Iterator { return bytes.makeIterator() }
    public var description: String { return "SHA512 digest: " + _hex(bytes) }
    public static func == (lhs: SHA512Digest, rhs: SHA512Digest) -> Bool { return _constantTimeEqual(lhs.bytes, rhs.bytes) }
    public func hash(into hasher: inout Hasher) { hasher.combine(bytes) }
}

/// An implementation of Secure Hashing Algorithm 2 (SHA-2) hashing with a 512-bit digest.
public struct SHA512: HashFunction {
    public typealias Digest = SHA512Digest
    internal var pending: [UInt8] = []
    public static var byteCount: Int { return 64 }
    public static var blockByteCount: Int { return 128 }
    public init() {}
    public mutating func update(bufferPointer: UnsafeRawBufferPointer) { pending.append(contentsOf: bufferPointer) }
    public func finalize() -> SHA512Digest { return SHA512Digest(bytes: _hostDigest(3, pending, 64)) }
}
