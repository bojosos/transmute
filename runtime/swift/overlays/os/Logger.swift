// Alchemy's project-authored os overlay (/usr/lib/swift/libswiftos.dylib):
// os_log_type_t's named values and os.Logger's construction, bounded by the
// symbols QLab 5.6.3 imports. Logger's logging entry points are
// transparent: applications compile them into os_log_type_enabled and
// _os_log_impl calls against libSystem.

@_exported import os // The Clang module (Alchemy's os_log declarations).

extension os_log_type_t {
    public static var `default`: os_log_type_t { return OS_LOG_TYPE_DEFAULT }
    public static var info: os_log_type_t { return OS_LOG_TYPE_INFO }
    public static var debug: os_log_type_t { return OS_LOG_TYPE_DEBUG }
    public static var error: os_log_type_t { return OS_LOG_TYPE_ERROR }
    public static var fault: os_log_type_t { return OS_LOG_TYPE_FAULT }
}

public typealias OSLogType = os_log_type_t

/// A value that logs messages to a subsystem and category.
public struct Logger: @unchecked Sendable {
    /// The underlying os_log object.
    public let logObject: OSLog

    public init(subsystem: String, category: String) {
        logObject = os_log_create(subsystem, category)
    }

    public init(_ logObj: OSLog) {
        logObject = logObj
    }
}
