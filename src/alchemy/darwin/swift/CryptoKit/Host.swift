// Alchemy's project-authored CryptoKit (/System/Library/Frameworks/
// CryptoKit.framework/Versions/A/CryptoKit): Apple's public CryptoKit API,
// bounded by what QLab 5.6.3 uses (ChaChaPoly, P521 key agreement, HKDF over
// SHA-2), written from Apple's documentation. The cryptography runs on the
// host (src/alchemy/darwin/security/crypto_core.cpp, tested against the
// RFC and FIPS vectors); these entry points are exported from the libSystem
// identity by crypto_exports.cpp. Every argument and result is a 64-bit
// integer or pointer, so Swift's calling convention passes them exactly as
// the C convention does (x0-x7, result in x0).

import Foundation

// Host status codes (crypto_exports.cpp).
internal let _statusOK = 0
internal let _statusIncorrectKeySize = 1
internal let _statusIncorrectParameterSize = 2
internal let _statusAuthenticationFailure = 3
internal let _statusInvalidKey = 4   // a scalar or point outside the curve's valid range

// Fills `count` bytes from the host's system random number generator.
@_silgen_name("alchemy_cryptokit_random")
internal func _alchemyRandom(_ buffer: UnsafeMutableRawPointer?, _ count: Int) -> Int

// algorithm: 1 SHA-256, 2 SHA-384, 3 SHA-512.
@_silgen_name("alchemy_cryptokit_hash")
internal func _alchemyHash(_ algorithm: Int, _ data: UnsafeRawPointer?, _ count: Int, _ digest: UnsafeMutableRawPointer?) -> Int

@_silgen_name("alchemy_cryptokit_hkdf_extract")
internal func _alchemyHKDFExtract(_ algorithm: Int, _ salt: UnsafeRawPointer?, _ saltCount: Int,
                                  _ key: UnsafeRawPointer?, _ keyCount: Int, _ prk: UnsafeMutableRawPointer?) -> Int

@_silgen_name("alchemy_cryptokit_hkdf_expand")
internal func _alchemyHKDFExpand(_ algorithm: Int, _ prk: UnsafeRawPointer?, _ prkCount: Int, _ info: UnsafeRawPointer?,
                                 _ infoCount: Int, _ output: UnsafeMutableRawPointer?, _ outputCount: Int) -> Int

// key: 32 bytes, nonce: 12 bytes, tag: 16 bytes.
@_silgen_name("alchemy_cryptokit_chachapoly_seal")
internal func _alchemyChaChaPolySeal(_ key: UnsafeRawPointer?, _ nonce: UnsafeRawPointer?, _ aad: UnsafeRawPointer?, _ aadCount: Int,
                                     _ plaintext: UnsafeRawPointer?, _ count: Int, _ ciphertext: UnsafeMutableRawPointer?,
                                     _ tag: UnsafeMutableRawPointer?) -> Int

@_silgen_name("alchemy_cryptokit_chachapoly_open")
internal func _alchemyChaChaPolyOpen(_ key: UnsafeRawPointer?, _ nonce: UnsafeRawPointer?, _ aad: UnsafeRawPointer?, _ aadCount: Int,
                                     _ ciphertext: UnsafeRawPointer?, _ count: Int, _ tag: UnsafeRawPointer?,
                                     _ plaintext: UnsafeMutableRawPointer?) -> Int

// Private keys: 66 bytes; public keys: X || Y, 132 bytes; secrets: 66 bytes.
@_silgen_name("alchemy_cryptokit_p521_generate")
internal func _alchemyP521Generate(_ compact: Int, _ privateKey: UnsafeMutableRawPointer?, _ publicKey: UnsafeMutableRawPointer?) -> Int

@_silgen_name("alchemy_cryptokit_p521_public_key")
internal func _alchemyP521PublicKey(_ privateKey: UnsafeRawPointer?, _ publicKey: UnsafeMutableRawPointer?) -> Int

@_silgen_name("alchemy_cryptokit_p521_validate")
internal func _alchemyP521Validate(_ publicKey: UnsafeRawPointer?) -> Int

@_silgen_name("alchemy_cryptokit_p521_shared_secret")
internal func _alchemyP521SharedSecret(_ privateKey: UnsafeRawPointer?, _ publicKey: UnsafeRawPointer?,
                                       _ secret: UnsafeMutableRawPointer?) -> Int

// Maps a host status to CryptoKit's error.
internal func _check(_ status: Int) throws {
    switch status {
    case _statusOK: return
    case _statusIncorrectKeySize: throw CryptoKitError.incorrectKeySize
    case _statusIncorrectParameterSize: throw CryptoKitError.incorrectParameterSize
    case _statusAuthenticationFailure: throw CryptoKitError.authenticationFailure
    case _statusInvalidKey: throw CryptoKitError.underlyingCoreCryptoError(error: -1)
    default: throw CryptoKitError.underlyingCoreCryptoError(error: Int32(truncatingIfNeeded: status))
    }
}

internal func _randomBytes(_ count: Int) -> [UInt8] {
    var bytes = [UInt8](repeating: 0, count: count)
    bytes.withUnsafeMutableBytes { buffer in
        let status = _alchemyRandom(buffer.baseAddress, buffer.count)
        precondition(status == _statusOK, "CryptoKit: the system random number generator failed")
    }
    return bytes
}

internal func _bytes<D: DataProtocol>(data: D) -> [UInt8] {
    return [UInt8](data)
}

internal func _bytes<C: ContiguousBytes>(contiguous: C) -> [UInt8] {
    return contiguous.withUnsafeBytes { [UInt8]($0) }
}

// Constant-time comparison of two byte strings.
internal func _constantTimeEqual(_ a: [UInt8], _ b: [UInt8]) -> Bool {
    guard a.count == b.count else { return false }
    var difference: UInt8 = 0
    for i in 0..<a.count { difference |= a[i] ^ b[i] }
    return difference == 0
}

internal func _hex(_ bytes: [UInt8]) -> String {
    let digits: [Character] = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f"]
    var text = ""
    for byte in bytes {
        text.append(digits[Int(byte >> 4)])
        text.append(digits[Int(byte & 0x0f)])
    }
    return text
}
