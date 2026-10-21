// SymmetricKey, SymmetricKeySize, SharedSecret and HKDF.

import Foundation

/// The sizes that a symmetric cryptographic key can take.
public struct SymmetricKeySize {
    /// The number of bits in the key.
    public let bitCount: Int

    /// Creates a new key size of the given length.
    public init(bitCount: Int) {
        precondition(bitCount > 0 && bitCount % 8 == 0, "SymmetricKeySize: the bit count must be a positive multiple of 8")
        self.bitCount = bitCount
    }

    /// A size of 128 bits.
    public static var bits128: SymmetricKeySize { return SymmetricKeySize(bitCount: 128) }
    /// A size of 192 bits.
    public static var bits192: SymmetricKeySize { return SymmetricKeySize(bitCount: 192) }
    /// A size of 256 bits.
    public static var bits256: SymmetricKeySize { return SymmetricKeySize(bitCount: 256) }
}

/// A symmetric cryptographic key.
public struct SymmetricKey: ContiguousBytes {
    internal let bytes: [UInt8]

    internal init(bytes: [UInt8]) { self.bytes = bytes }

    /// Creates a key from the given data.
    public init<D: ContiguousBytes>(data: D) {
        self.bytes = _bytes(contiguous: data)
    }

    /// Generates a new random key of the given size.
    public init(size: SymmetricKeySize) {
        self.bytes = _randomBytes(size.bitCount / 8)
    }

    /// The number of bits in the key.
    public var bitCount: Int { return bytes.count * 8 }

    /// Invokes the given closure with a buffer pointer to the key's underlying bytes.
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try bytes.withUnsafeBytes(body)
    }
}

extension SymmetricKey: Equatable {
    /// Determines whether two keys are equal (in constant time).
    public static func == (lhs: SymmetricKey, rhs: SymmetricKey) -> Bool {
        return _constantTimeEqual(lhs.bytes, rhs.bytes)
    }
}

/// A key agreement result from which you can derive a symmetric cryptographic key.
public struct SharedSecret: ContiguousBytes {
    internal let bytes: [UInt8]

    internal init(bytes: [UInt8]) { self.bytes = bytes }

    /// Invokes the given closure with a buffer pointer to the secret's underlying bytes.
    public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        return try bytes.withUnsafeBytes(body)
    }

    /// Derives a symmetric encryption key from the secret using HKDF key derivation (RFC 5869).
    public func hkdfDerivedSymmetricKey<H: HashFunction, Salt: DataProtocol, SI: DataProtocol>(
        using hashFunction: H.Type, salt: Salt, sharedInfo: SI, outputByteCount: Int) -> SymmetricKey {
        return SymmetricKey(bytes: _hkdf(H.self, key: bytes, salt: _bytes(data: salt), info: _bytes(data: sharedInfo),
                                         outputByteCount: outputByteCount))
    }
}

extension SharedSecret: Equatable {
    /// Determines whether two shared secrets are equal (in constant time).
    public static func == (lhs: SharedSecret, rhs: SharedSecret) -> Bool {
        return _constantTimeEqual(lhs.bytes, rhs.bytes)
    }
}

/// A standards-based implementation of an HMAC-based Key Derivation Function (HKDF).
public struct HKDF<H: HashFunction> {
    /// Derives a symmetric encryption key from the input key material, salt and info.
    public static func deriveKey<Salt: DataProtocol, Info: DataProtocol>(
        inputKeyMaterial: SymmetricKey, salt: Salt, info: Info, outputByteCount: Int) -> SymmetricKey {
        return SymmetricKey(bytes: _hkdf(H.self, key: inputKeyMaterial.bytes, salt: _bytes(data: salt), info: _bytes(data: info),
                                         outputByteCount: outputByteCount))
    }

    /// Derives a symmetric encryption key from the input key material and info, without a salt.
    public static func deriveKey<Info: DataProtocol>(inputKeyMaterial: SymmetricKey, info: Info, outputByteCount: Int) -> SymmetricKey {
        return SymmetricKey(bytes: _hkdf(H.self, key: inputKeyMaterial.bytes, salt: [], info: _bytes(data: info),
                                         outputByteCount: outputByteCount))
    }

    /// Derives a symmetric encryption key from the input key material, without a salt or info.
    public static func deriveKey(inputKeyMaterial: SymmetricKey, outputByteCount: Int) -> SymmetricKey {
        return SymmetricKey(bytes: _hkdf(H.self, key: inputKeyMaterial.bytes, salt: [], info: [], outputByteCount: outputByteCount))
    }
}

// HKDF-Extract then HKDF-Expand. CryptoKit's SHA-2 functions run on the host;
// a hash function defined elsewhere goes through HMAC built on its
// HashFunction requirements.
internal func _hkdf<H: HashFunction>(_ hash: H.Type, key: [UInt8], salt: [UInt8], info: [UInt8], outputByteCount: Int) -> [UInt8] {
    let digestCount = H.Digest.byteCount
    precondition(outputByteCount >= 0 && outputByteCount <= 255 * digestCount,
                 "HKDF: the output byte count must be between 0 and 255 times the digest size")
    if let algorithm = _hostAlgorithm(hash) {
        var prk = [UInt8](repeating: 0, count: digestCount)
        var status = salt.withUnsafeBytes { saltBuffer in
            key.withUnsafeBytes { keyBuffer in
                prk.withUnsafeMutableBytes { prkBuffer in
                    _alchemyHKDFExtract(algorithm, saltBuffer.baseAddress, saltBuffer.count, keyBuffer.baseAddress, keyBuffer.count,
                                        prkBuffer.baseAddress)
                }
            }
        }
        precondition(status == _statusOK, "HKDF: extraction failed")
        var output = [UInt8](repeating: 0, count: outputByteCount)
        status = prk.withUnsafeBytes { prkBuffer in
            info.withUnsafeBytes { infoBuffer in
                output.withUnsafeMutableBytes { outputBuffer in
                    _alchemyHKDFExpand(algorithm, prkBuffer.baseAddress, prkBuffer.count, infoBuffer.baseAddress, infoBuffer.count,
                                       outputBuffer.baseAddress, outputBuffer.count)
                }
            }
        }
        precondition(status == _statusOK, "HKDF: expansion failed")
        return output
    }
    let prk = _hmac(hash, key: salt.isEmpty ? [UInt8](repeating: 0, count: digestCount) : salt, message: key)
    var output: [UInt8] = []
    var block: [UInt8] = []
    var counter: UInt8 = 1
    while output.count < outputByteCount {
        block = _hmac(hash, key: prk, message: block + info + [counter])
        output += block
        counter &+= 1
    }
    return Array(output.prefix(outputByteCount))
}

// HMAC (RFC 2104) over a HashFunction's requirements.
internal func _hmac<H: HashFunction>(_ hash: H.Type, key: [UInt8], message: [UInt8]) -> [UInt8] {
    var padded = key.count > H.blockByteCount ? _digest(hash, key) : key
    padded += [UInt8](repeating: 0, count: H.blockByteCount - padded.count)
    let inner = _digest(hash, padded.map { $0 ^ 0x36 } + message)
    return _digest(hash, padded.map { $0 ^ 0x5c } + inner)
}

internal func _digest<H: HashFunction>(_ hash: H.Type, _ bytes: [UInt8]) -> [UInt8] {
    var hasher = H()
    bytes.withUnsafeBytes { hasher.update(bufferPointer: $0) }
    return Array(hasher.finalize())
}
