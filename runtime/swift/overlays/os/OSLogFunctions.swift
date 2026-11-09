// os_log's Swift functions and OSLog.default (project-authored, from Apple's
// public os documentation). The message and its arguments go to libSystem's
// os_log_with_args as a C va_list, which formats them (with %@ and os_log's
// {public}/{private} annotations) like any other os_log call.

extension OSLog {
    /// The default log object (OS_LOG_DEFAULT).
    public static var `default`: OSLog {
        return unsafe withUnsafeMutablePointer(to: &_os_log_default) { pointer in
            unsafe Unmanaged<OSLog>.fromOpaque(UnsafeRawPointer(pointer)).takeUnretainedValue()
        }
    }
}

public func os_log(_ message: StaticString, dso: UnsafeRawPointer = #dsohandle, log: OSLog = .default,
                   type: OSLogType = .default, _ args: CVarArg...) {
    _osLog(message, dso: dso, log: log, type: type, args)
}

public func os_log(_ type: OSLogType, dso: UnsafeRawPointer = #dsohandle, log: OSLog = .default, _ message: StaticString,
                   _ args: CVarArg...) {
    _osLog(message, dso: dso, log: log, type: type, args)
}

private func _osLog(_ message: StaticString, dso: UnsafeRawPointer, log: OSLog, type: OSLogType, _ args: [CVarArg]) {
    guard os_log_type_enabled(log, type) else { return }
    // The calling image's header stands in for the return address: both
    // name the image the message comes from.
    unsafe message.withUTF8Buffer { format in
        unsafe format.withMemoryRebound(to: CChar.self) { text in
            let terminated = Array(text) + [0]
            unsafe terminated.withUnsafeBufferPointer { cString in
                unsafe withVaList(args) { list in
                    unsafe os_log_with_args(log, type, cString.baseAddress!, list, UnsafeMutableRawPointer(mutating: dso))
                }
            }
        }
    }
}
