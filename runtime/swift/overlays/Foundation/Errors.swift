// Foundation's error protocols (project-authored, from Apple's public Foundation documentation): LocalizedError, the
// localized descriptions an error can give, each nil unless the error provides it.

/// An error that gives localized messages describing why it occurred and how to recover.
public protocol LocalizedError: Error {
    /// What the error is.
    var errorDescription: String? { get }
    /// Why the error occurred.
    var failureReason: String? { get }
    /// How to recover from the error.
    var recoverySuggestion: String? { get }
    /// The help anchor for more about the error.
    var helpAnchor: String? { get }
}

extension LocalizedError {
    public var errorDescription: String? { nil }
    public var failureReason: String? { nil }
    public var recoverySuggestion: String? { nil }
    public var helpAnchor: String? { nil }
}
