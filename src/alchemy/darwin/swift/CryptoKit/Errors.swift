// CryptoKitError, in Apple's documented case order.

/// General cryptography errors used by CryptoKit.
public enum CryptoKitError: Error {
    /// The key size is incorrect.
    case incorrectKeySize
    /// The parameter size is incorrect.
    case incorrectParameterSize
    /// The authentication tag or signature is incorrect.
    case authenticationFailure
    /// The underlying cryptography library failed with the given error code.
    case underlyingCoreCryptoError(error: Int32)
    /// Failed to wrap the key.
    case wrapFailure
    /// Failed to unwrap the key.
    case unwrapFailure
    /// The parameter is invalid.
    case invalidParameter
}
