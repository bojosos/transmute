// ChaChaPoly: the ChaCha20-Poly1305 AEAD (RFC 8439) with a 256-bit key, a
// 96-bit nonce and a 128-bit tag. A sealed box's combined representation is
// nonce || ciphertext || tag.

import Foundation

/// An implementation of the ChaCha20-Poly1305 cipher.
public enum ChaChaPoly {
    internal static let nonceByteCount = 12
    internal static let tagByteCount = 16
    internal static let keyByteCount = 32

    /// A value used once during a cryptographic operation and then discarded.
    public struct Nonce: ContiguousBytes, Sequence {
        internal let bytes: [UInt8]

        /// Creates a new random nonce.
        public init() {
            self.bytes = _randomBytes(ChaChaPoly.nonceByteCount)
        }

        /// Creates a nonce from the given data (12 bytes).
        public init<D: DataProtocol>(data: D) throws {
            let bytes = _bytes(data: data)
            guard bytes.count == ChaChaPoly.nonceByteCount else { throw CryptoKitError.incorrectParameterSize }
            self.bytes = bytes
        }

        public func withUnsafeBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
            return try bytes.withUnsafeBytes(body)
        }

        public func makeIterator() -> Array<UInt8>.Iterator { return bytes.makeIterator() }
    }

    /// A secure container for your data that you access using a cipher.
    public struct SealedBox {
        internal let nonceBytes: [UInt8]
        internal let ciphertextBytes: [UInt8]
        internal let tagBytes: [UInt8]

        internal init(nonceBytes: [UInt8], ciphertextBytes: [UInt8], tagBytes: [UInt8]) {
            self.nonceBytes = nonceBytes
            self.ciphertextBytes = ciphertextBytes
            self.tagBytes = tagBytes
        }

        /// Creates a sealed box from the combined bytes of an authentication tag, nonce, and encrypted payload.
        public init<D: DataProtocol>(combined: D) throws {
            let bytes = _bytes(data: combined)
            guard bytes.count >= ChaChaPoly.nonceByteCount + ChaChaPoly.tagByteCount else {
                throw CryptoKitError.incorrectParameterSize
            }
            nonceBytes = Array(bytes[0..<ChaChaPoly.nonceByteCount])
            ciphertextBytes = Array(bytes[ChaChaPoly.nonceByteCount..<(bytes.count - ChaChaPoly.tagByteCount)])
            tagBytes = Array(bytes[(bytes.count - ChaChaPoly.tagByteCount)...])
        }

        /// Creates a sealed box from the given nonce, ciphertext and authentication tag.
        public init<C: DataProtocol, T: DataProtocol>(nonce: ChaChaPoly.Nonce, ciphertext: C, tag: T) throws {
            let tag = _bytes(data: tag)
            guard tag.count == ChaChaPoly.tagByteCount else { throw CryptoKitError.incorrectParameterSize }
            self.init(nonceBytes: nonce.bytes, ciphertextBytes: _bytes(data: ciphertext), tagBytes: tag)
        }

        /// A combined element composed of the nonce, encrypted data, and authentication tag.
        public var combined: Data { return Data(nonceBytes + ciphertextBytes + tagBytes) }

        /// The nonce used to encrypt the data.
        public var nonce: ChaChaPoly.Nonce { return try! ChaChaPoly.Nonce(data: nonceBytes) }

        /// The encrypted data.
        public var ciphertext: Data { return Data(ciphertextBytes) }

        /// An authentication tag.
        public var tag: Data { return Data(tagBytes) }
    }

    /// Encrypts and authenticates data using ChaCha20-Poly1305.
    public static func seal<Plaintext: DataProtocol>(_ message: Plaintext, using key: SymmetricKey, nonce: Nonce? = nil) throws -> SealedBox {
        return try _seal(_bytes(data: message), key: key, nonce: nonce, aad: [])
    }

    /// Encrypts and authenticates data using ChaCha20-Poly1305, also authenticating additional data.
    public static func seal<Plaintext: DataProtocol, AuthenticatedData: DataProtocol>(
        _ message: Plaintext, using key: SymmetricKey, nonce: Nonce? = nil, authenticating authenticatedData: AuthenticatedData) throws -> SealedBox {
        return try _seal(_bytes(data: message), key: key, nonce: nonce, aad: _bytes(data: authenticatedData))
    }

    /// Decrypts the message and verifies its authenticity using ChaCha20-Poly1305.
    public static func open(_ sealedBox: SealedBox, using key: SymmetricKey) throws -> Data {
        return Data(try _open(sealedBox, key: key, aad: []))
    }

    /// Decrypts the message and verifies the authenticity of both the encrypted message and additional data.
    public static func open<AuthenticatedData: DataProtocol>(
        _ sealedBox: SealedBox, using key: SymmetricKey, authenticating authenticatedData: AuthenticatedData) throws -> Data {
        return Data(try _open(sealedBox, key: key, aad: _bytes(data: authenticatedData)))
    }

    internal static func _seal(_ plaintext: [UInt8], key: SymmetricKey, nonce: Nonce?, aad: [UInt8]) throws -> SealedBox {
        guard key.bytes.count == keyByteCount else { throw CryptoKitError.incorrectKeySize }
        let nonceBytes = (nonce ?? Nonce()).bytes
        var ciphertext = [UInt8](repeating: 0, count: plaintext.count)
        var tag = [UInt8](repeating: 0, count: tagByteCount)
        try _check(key.bytes.withUnsafeBytes { keyBuffer in
            nonceBytes.withUnsafeBytes { nonceBuffer in
                aad.withUnsafeBytes { aadBuffer in
                    plaintext.withUnsafeBytes { plaintextBuffer in
                        ciphertext.withUnsafeMutableBytes { ciphertextBuffer in
                            tag.withUnsafeMutableBytes { tagBuffer in
                                _alchemyChaChaPolySeal(keyBuffer.baseAddress, nonceBuffer.baseAddress, aadBuffer.baseAddress, aadBuffer.count,
                                                       plaintextBuffer.baseAddress, plaintextBuffer.count, ciphertextBuffer.baseAddress,
                                                       tagBuffer.baseAddress)
                            }
                        }
                    }
                }
            }
        })
        return SealedBox(nonceBytes: nonceBytes, ciphertextBytes: ciphertext, tagBytes: tag)
    }

    internal static func _open(_ box: SealedBox, key: SymmetricKey, aad: [UInt8]) throws -> [UInt8] {
        guard key.bytes.count == keyByteCount else { throw CryptoKitError.incorrectKeySize }
        var plaintext = [UInt8](repeating: 0, count: box.ciphertextBytes.count)
        try _check(key.bytes.withUnsafeBytes { keyBuffer in
            box.nonceBytes.withUnsafeBytes { nonceBuffer in
                aad.withUnsafeBytes { aadBuffer in
                    box.ciphertextBytes.withUnsafeBytes { ciphertextBuffer in
                        box.tagBytes.withUnsafeBytes { tagBuffer in
                            plaintext.withUnsafeMutableBytes { plaintextBuffer in
                                _alchemyChaChaPolyOpen(keyBuffer.baseAddress, nonceBuffer.baseAddress, aadBuffer.baseAddress, aadBuffer.count,
                                                       ciphertextBuffer.baseAddress, ciphertextBuffer.count, tagBuffer.baseAddress,
                                                       plaintextBuffer.baseAddress)
                            }
                        }
                    }
                }
            }
        })
        return plaintext
    }
}
