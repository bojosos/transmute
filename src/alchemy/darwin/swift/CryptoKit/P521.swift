// P521 key agreement (ECDH over NIST P-521). Raw representations follow
// CryptoKit: a private key is the 66-byte big-endian scalar, a public key is
// X || Y (132 bytes), and the shared secret is the 66-byte x-coordinate.

import Foundation

/// An elliptic curve that enables NIST P-521 signatures and key agreement.
public enum P521 {
    /// A mechanism used to create a shared secret between two users by performing NIST P-521 elliptic curve
    /// Diffie Hellman (ECDH) key exchange.
    public enum KeyAgreement {
        /// A P-521 public key used for key agreement.
        public struct PublicKey {
            internal let raw: [UInt8]

            internal init(validated raw: [UInt8]) { self.raw = raw }

            /// Creates a P-521 public key for key agreement from a collection of bytes (X || Y).
            public init<D: ContiguousBytes>(rawRepresentation: D) throws {
                let raw = _bytes(contiguous: rawRepresentation)
                guard raw.count == 132 else { throw CryptoKitError.incorrectKeySize }
                try _check(raw.withUnsafeBytes { _alchemyP521Validate($0.baseAddress) })
                self.raw = raw
            }

            /// Creates a P-521 public key for key agreement from an ANSI x9.63 representation (0x04 || X || Y).
            public init<Bytes: ContiguousBytes>(x963Representation: Bytes) throws {
                let encoded = _bytes(contiguous: x963Representation)
                guard encoded.count == 133 else { throw CryptoKitError.incorrectKeySize }
                guard encoded[0] == 0x04 else { throw CryptoKitError.invalidParameter }
                try self.init(rawRepresentation: Array(encoded[1...]))
            }

            /// A full representation of the public key: the X and Y coordinates.
            public var rawRepresentation: Data { return Data(raw) }

            /// An ANSI x9.63 representation of the public key (0x04 || X || Y).
            public var x963Representation: Data { return Data([0x04] + raw) }
        }

        /// A P-521 private key used for key agreement.
        public struct PrivateKey {
            internal let scalar: [UInt8]
            internal let publicRaw: [UInt8]

            /// Creates a random P-521 private key for key agreement; with `compactRepresentable`, one whose
            /// public key has a compact representation (y <= p - y).
            public init(compactRepresentable: Bool = true) {
                var scalar = [UInt8](repeating: 0, count: 66)
                var publicRaw = [UInt8](repeating: 0, count: 132)
                let status = scalar.withUnsafeMutableBytes { scalarBuffer in
                    publicRaw.withUnsafeMutableBytes { publicBuffer in
                        _alchemyP521Generate(compactRepresentable ? 1 : 0, scalarBuffer.baseAddress, publicBuffer.baseAddress)
                    }
                }
                precondition(status == _statusOK, "P521: key generation failed")
                self.scalar = scalar
                self.publicRaw = publicRaw
            }

            /// Creates a P-521 private key for key agreement from a collection of bytes (the 66-byte scalar).
            public init<D: ContiguousBytes>(rawRepresentation: D) throws {
                let scalar = _bytes(contiguous: rawRepresentation)
                guard scalar.count == 66 else { throw CryptoKitError.incorrectKeySize }
                var publicRaw = [UInt8](repeating: 0, count: 132)
                try _check(scalar.withUnsafeBytes { scalarBuffer in
                    publicRaw.withUnsafeMutableBytes { publicBuffer in
                        _alchemyP521PublicKey(scalarBuffer.baseAddress, publicBuffer.baseAddress)
                    }
                })
                self.scalar = scalar
                self.publicRaw = publicRaw
            }

            /// Creates a P-521 private key for key agreement from an ANSI x9.63 representation
            /// (0x04 || X || Y || K).
            public init<Bytes: ContiguousBytes>(x963Representation: Bytes) throws {
                let encoded = _bytes(contiguous: x963Representation)
                guard encoded.count == 1 + 132 + 66 else { throw CryptoKitError.incorrectKeySize }
                guard encoded[0] == 0x04 else { throw CryptoKitError.invalidParameter }
                try self.init(rawRepresentation: Array(encoded[133...]))
                guard _constantTimeEqual(publicRaw, Array(encoded[1..<133])) else { throw CryptoKitError.invalidParameter }
            }

            /// The corresponding public key.
            public var publicKey: P521.KeyAgreement.PublicKey { return PublicKey(validated: publicRaw) }

            /// A data representation of the private key (the 66-byte scalar).
            public var rawRepresentation: Data { return Data(scalar) }

            /// An ANSI x9.63 representation of the private key (0x04 || X || Y || K).
            public var x963Representation: Data { return Data([0x04] + publicRaw + scalar) }

            /// Computes a shared secret with the provided public key from another party.
            public func sharedSecretFromKeyAgreement(with publicKeyShare: P521.KeyAgreement.PublicKey) throws -> SharedSecret {
                var secret = [UInt8](repeating: 0, count: 66)
                try _check(scalar.withUnsafeBytes { scalarBuffer in
                    publicKeyShare.raw.withUnsafeBytes { publicBuffer in
                        secret.withUnsafeMutableBytes { secretBuffer in
                            _alchemyP521SharedSecret(scalarBuffer.baseAddress, publicBuffer.baseAddress, secretBuffer.baseAddress)
                        }
                    }
                })
                return SharedSecret(bytes: secret)
            }
        }
    }
}
