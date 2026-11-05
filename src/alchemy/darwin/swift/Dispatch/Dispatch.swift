// Alchemy's project-authored Dispatch overlay (/usr/lib/swift/
// libswiftDispatch.dylib): Apple's Darwin Dispatch Swift API over the
// Objective-C dispatch classes (OS_dispatch_queue as DispatchQueue, ...),
// bounded by what QLab 5.6.3 uses: DispatchQoS, DispatchWorkItemFlags,
// DispatchQueue.Attributes and AutoreleaseFrequency, DispatchQueue.main,
// init(label:qos:attributes:autoreleaseFrequency:target:) and
// async(group:qos:flags:execute:); and what Maccy and Pika (SwiftUI apps) add:
// DispatchTime (now, +), DispatchWorkItem, asyncAfter, global(qos:),
// DispatchGroup.notify and the file-system object source. Written from
// Apple's public documentation.
// QoS classes and the QoS-related work item flags are scheduling hints:
// Alchemy's libdispatch runs work on host threads without QoS, so they do
// not change where or when work runs; the barrier flag is honored.

@_exported import Dispatch // The Clang module (Alchemy's dispatch declarations).

// ---- DispatchQoS -----------------------------------------------------------------

/// The quality of service, or the execution priority, to apply to tasks.
public struct DispatchQoS: Equatable {
    /// The quality-of-service class.
    public let qosClass: QoSClass
    /// The relative priority within the QoS class (0 or negative, down to -15).
    public let relativePriority: Int

    /// The quality-of-service class for maintenance or cleanup tasks.
    public static let background = DispatchQoS(qosClass: .background, relativePriority: 0)
    /// The quality-of-service class for tasks the user does not track actively.
    public static let utility = DispatchQoS(qosClass: .utility, relativePriority: 0)
    /// The default quality-of-service class.
    public static let `default` = DispatchQoS(qosClass: .default, relativePriority: 0)
    /// The quality-of-service class for tasks that prevent the user from actively using your app.
    public static let userInitiated = DispatchQoS(qosClass: .userInitiated, relativePriority: 0)
    /// The quality-of-service class for user-interactive tasks.
    public static let userInteractive = DispatchQoS(qosClass: .userInteractive, relativePriority: 0)
    /// The absence of a quality-of-service class.
    public static let unspecified = DispatchQoS(qosClass: .unspecified, relativePriority: 0)

    /// Quality-of-service classes that specify the priorities for executing tasks.
    public enum QoSClass {
        case background
        case utility
        case `default`
        case userInitiated
        case userInteractive
        case unspecified

        /// Creates a QoS class from the given system QoS value.
        public init?(rawValue: qos_class_t) {
            switch rawValue {
            case QOS_CLASS_BACKGROUND: self = .background
            case QOS_CLASS_UTILITY: self = .utility
            case QOS_CLASS_DEFAULT: self = .default
            case QOS_CLASS_USER_INITIATED: self = .userInitiated
            case QOS_CLASS_USER_INTERACTIVE: self = .userInteractive
            case QOS_CLASS_UNSPECIFIED: self = .unspecified
            default: return nil
            }
        }

        /// The system QoS value.
        public var rawValue: qos_class_t {
            switch self {
            case .background: return QOS_CLASS_BACKGROUND
            case .utility: return QOS_CLASS_UTILITY
            case .default: return QOS_CLASS_DEFAULT
            case .userInitiated: return QOS_CLASS_USER_INITIATED
            case .userInteractive: return QOS_CLASS_USER_INTERACTIVE
            case .unspecified: return QOS_CLASS_UNSPECIFIED
            }
        }
    }

    /// Creates a quality-of-service value with the given class and relative priority.
    public init(qosClass: QoSClass, relativePriority: Int) {
        self.qosClass = qosClass
        self.relativePriority = relativePriority
    }
}

// ---- DispatchWorkItemFlags -----------------------------------------------------------

/// A set of behaviors for a work item, such as its quality-of-service class and whether to create a barrier.
public struct DispatchWorkItemFlags: OptionSet, RawRepresentable {
    public let rawValue: UInt
    public init(rawValue: UInt) { self.rawValue = rawValue }

    /// Cause the work item to act as a barrier block when submitted to a concurrent queue.
    public static let barrier = DispatchWorkItemFlags(rawValue: 0x1)
    /// Disassociate the work item's attributes from the current execution context.
    public static let detached = DispatchWorkItemFlags(rawValue: 0x2)
    /// Set the attributes of the work item to match the attributes of the current execution context.
    public static let assignCurrentContext = DispatchWorkItemFlags(rawValue: 0x4)
    /// Execute the work item without assigning a quality-of-service class.
    public static let noQoS = DispatchWorkItemFlags(rawValue: 0x8)
    /// Prefer the quality-of-service class associated with the current execution context.
    public static let inheritQoS = DispatchWorkItemFlags(rawValue: 0x10)
    /// Prefer the quality-of-service class associated with the block.
    public static let enforceQoS = DispatchWorkItemFlags(rawValue: 0x20)
}

// ---- DispatchQueue -------------------------------------------------------------------

extension DispatchQueue {
    /// Attributes that define the behavior of a dispatch queue.
    public struct Attributes: OptionSet {
        public let rawValue: UInt64
        public init(rawValue: UInt64) { self.rawValue = rawValue }

        /// The dispatch queue executes tasks concurrently.
        public static let concurrent = Attributes(rawValue: 1 << 1)
        /// The newly created dispatch queue is inactive until activated.
        public static let initiallyInactive = Attributes(rawValue: 1 << 2)

        fileprivate func _attr() -> UnsafeMutableRawPointer? {
            var attr: UnsafeMutableRawPointer? = nil
            if contains(.concurrent) { attr = ___alchemy_dispatch_queue_concurrent() }
            if contains(.initiallyInactive) {
                precondition(___alchemy_dispatch_has_initially_inactive(),
                             "unsupported: Dispatch: dispatch_queue_attr_make_initially_inactive is not provided")
                attr = __dispatch_queue_attr_make_initially_inactive(attr)
            }
            return attr
        }
    }

    /// Constants that indicate how a dispatch queue manages autorelease pools for the blocks it executes.
    public enum AutoreleaseFrequency {
        /// The queue inherits its autorelease frequency from its target queue.
        case inherit
        /// The queue configures an autorelease pool before executing a block, and releases the objects in that pool after the block finishes.
        case workItem
        /// The queue does not set up an autorelease pool around executed blocks.
        case never

        fileprivate var _value: UInt {
            switch self {
            case .inherit: return 0
            case .workItem: return 1
            case .never: return 2
            }
        }
    }

    /// The dispatch queue associated with the main thread of the current process.
    public class var main: DispatchQueue {
        return Unmanaged<DispatchQueue>.fromOpaque(___alchemy_dispatch_main_queue()).takeUnretainedValue()
    }

    /// Creates a new dispatch queue to which you can submit blocks.
    public convenience init(label: String, qos: DispatchQoS = .unspecified, attributes: Attributes = [],
                            autoreleaseFrequency: AutoreleaseFrequency = .inherit, target: DispatchQueue? = nil) {
        var attr = attributes._attr()
        if autoreleaseFrequency != .inherit {
            precondition(___alchemy_dispatch_has_autorelease_frequency(),
                         "unsupported: Dispatch: dispatch_queue_attr_make_with_autorelease_frequency is not provided")
            attr = __dispatch_queue_attr_make_with_autorelease_frequency(attr, autoreleaseFrequency._value)
        }
        if qos != .unspecified {
            attr = __dispatch_queue_attr_make_with_qos_class(attr, qos.qosClass.rawValue, Int32(qos.relativePriority))
        }
        self.init(__label: label, attr: attr)
        if let target = target { __dispatch_set_target_queue(self, target) }
    }

    /// The label you assigned to the dispatch queue at creation time.
    public var label: String {
        return String(cString: __dispatch_queue_get_label(self))
    }

    /// Schedules a block asynchronously for execution and optionally associates it with a dispatch group.
    public func async(group: DispatchGroup? = nil, qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [],
                      execute work: @escaping @convention(block) () -> Void) {
        let barrier = flags.contains(.barrier)
        if let group = group {
            if barrier {
                __dispatch_group_enter(group)
                __dispatch_barrier_async(self) {
                    work()
                    __dispatch_group_leave(group)
                }
            } else {
                __dispatch_group_async(group, self, work)
            }
        } else if barrier {
            __dispatch_barrier_async(self, work)
        } else {
            __dispatch_async(self, work)
        }
    }

    /// Schedules a block for execution at the specified time, and returns immediately.
    public func asyncAfter(deadline: DispatchTime, qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [],
                           execute work: @escaping @convention(block) () -> Void) {
        if flags.isEmpty { __dispatch_after(deadline.rawValue, self, work); return }
        _withFlagged(work, flags) { ___alchemy_dispatch_after(deadline.rawValue, self, $0) }
    }

    /// Schedules a work item for execution at the specified time, and returns immediately.
    public func asyncAfter(deadline: DispatchTime, execute: DispatchWorkItem) {
        ___alchemy_dispatch_after(deadline.rawValue, self, execute._block)
    }

    /// Returns the global system queue with the specified quality-of-service class.
    public static func global(qos: DispatchQoS.QoSClass = .default) -> DispatchQueue {
        return __dispatch_get_global_queue(Int(qos.rawValue.rawValue), 0)
    }
}

// ---- DispatchTime ------------------------------------------------------------------

/// A point in time relative to the default clock, with nanosecond precision.
public struct DispatchTime: Comparable {
    /// The raw dispatch_time_t value.
    public let rawValue: dispatch_time_t

    /// A time in the distant future.
    public static let distantFuture = DispatchTime(rawValue: ~0)

    /// Returns the current time.
    public static func now() -> DispatchTime {
        return DispatchTime(rawValue: __dispatch_time(0, 0))   // DISPATCH_TIME_NOW
    }

    /// Creates a time from a raw dispatch_time_t value.
    public init(rawValue: dispatch_time_t) { self.rawValue = rawValue }

    public static func < (a: DispatchTime, b: DispatchTime) -> Bool { return a.rawValue < b.rawValue }
}

/// Returns a time the given number of seconds after a time (the distant future for one beyond the clock's range).
public func + (time: DispatchTime, seconds: Double) -> DispatchTime {
    precondition(!seconds.isNaN, "Dispatch: a NaN interval")
    let nanoseconds = seconds * 1_000_000_000
    if nanoseconds >= Double(Int64.max) { return .distantFuture }
    let delta = nanoseconds <= Double(Int64.min) ? Int64.min : Int64(nanoseconds)
    return DispatchTime(rawValue: __dispatch_time(time.rawValue, delta))
}

// ---- DispatchWorkItem ----------------------------------------------------------------

/// The work you want to perform, encapsulated in a way that lets you attach a completion handle or execution
/// dependencies, and cancel it before it runs.
public class DispatchWorkItem {
    /// The dispatch block (dispatch_block_create) the queues run, held by pointer to keep its identity; owned.
    internal let _block: UnsafeMutableRawPointer

    /// Creates a work item with the given flags and block.
    public init(flags: DispatchWorkItemFlags, block: @escaping @convention(block) () -> Void) {
        guard let created = ___alchemy_dispatch_block_create(UInt(flags.rawValue), block) else {
            preconditionFailure("Dispatch: dispatch_block_create failed")
        }
        _block = created
    }

    deinit { ___alchemy_block_release(_block) }

    /// Creates a work item with the given quality-of-service class, flags and block.
    public convenience init(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [],
                            block: @escaping @convention(block) () -> Void) {
        self.init(flags: flags, block: block)   // QoS is a scheduling hint here (see above)
    }

    /// Executes the work item's block synchronously on the current thread.
    public func perform() {
        // A block is called through its invoke function (at offset 16) with the block itself.
        let invoke = _block.load(fromByteOffset: 16, as: (@convention(c) (UnsafeMutableRawPointer) -> Void).self)
        invoke(_block)
    }

    /// Cancels the work item: if it has not started yet, it does not run.
    public func cancel() { ___alchemy_dispatch_block_cancel(_block) }

    /// Whether the work item was cancelled.
    public var isCancelled: Bool { return ___alchemy_dispatch_block_testcancel(_block) != 0 }
}

// ---- DispatchGroup -------------------------------------------------------------------

extension DispatchGroup {
    /// Schedules a block to run on a queue when every task in the group has finished.
    public func notify(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], queue: DispatchQueue,
                       execute work: @escaping @convention(block) () -> Void) {
        if flags.isEmpty { __dispatch_group_notify(self, queue, work); return }
        _withFlagged(work, flags) { ___alchemy_dispatch_group_notify(self, queue, $0) }
    }
}

/// Passes a dispatch block that carries work item flags to `use` (which copies it if it keeps it), then releases it.
private func _withFlagged(_ work: @escaping @convention(block) () -> Void, _ flags: DispatchWorkItemFlags,
                          _ use: (UnsafeMutableRawPointer) -> Void) {
    guard let block = ___alchemy_dispatch_block_create(UInt(flags.rawValue), work) else {
        preconditionFailure("Dispatch: dispatch_block_create failed")
    }
    use(block)
    ___alchemy_block_release(block)
}

// ---- Dispatch sources ----------------------------------------------------------------

/// The type of a dispatch source's event and cancellation handlers.
public typealias DispatchSourceHandler = @convention(block) () -> Void

/// A source's object as the class every dispatch source is (protocol existentials of sources are that object).
private func _dispatchSource(_ object: AnyObject) -> DispatchSource {
    return unsafeBitCast(object, to: DispatchSource.self)
}

extension DispatchSource {
    /// The events a file-system object source monitors (DISPATCH_VNODE_*).
    public struct FileSystemEvent: OptionSet, RawRepresentable {
        public let rawValue: UInt
        public init(rawValue: UInt) { self.rawValue = rawValue }

        /// The file-system object was deleted from the namespace.
        public static let delete = FileSystemEvent(rawValue: 0x1)
        /// The file-system object's data changed.
        public static let write = FileSystemEvent(rawValue: 0x2)
        /// The file-system object changed in size.
        public static let extend = FileSystemEvent(rawValue: 0x4)
        /// The file-system object's metadata changed.
        public static let attrib = FileSystemEvent(rawValue: 0x8)
        /// The file-system object's link count changed.
        public static let link = FileSystemEvent(rawValue: 0x10)
        /// The file-system object was renamed in the namespace.
        public static let rename = FileSystemEvent(rawValue: 0x20)
        /// The file-system object was revoked.
        public static let revoke = FileSystemEvent(rawValue: 0x40)
        /// The file-system object was unlocked.
        public static let funlock = FileSystemEvent(rawValue: 0x100)
    }

    /// Creates a dispatch source that monitors a file descriptor for the given events.
    public static func makeFileSystemObjectSource(fileDescriptor: Int32, eventMask: FileSystemEvent,
                                                  queue: DispatchQueue? = nil) -> DispatchSourceFileSystemObject {
        let source = __dispatch_source_create(___alchemy_dispatch_source_type_vnode(), UInt(bitPattern: Int(fileDescriptor)),
                                              eventMask.rawValue, queue)
        return unsafeBitCast(source, to: DispatchSourceFileSystemObject.self)
    }
}

extension DispatchSourceProtocol {
    /// Sets the event handler block for the dispatch source.
    public func setEventHandler(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], handler: DispatchSourceHandler?) {
        let source = _dispatchSource(self)
        if let handler = handler, !flags.isEmpty {
            _withFlagged(handler, flags) { ___alchemy_dispatch_source_set_event_handler(source, $0) }
        } else {
            __dispatch_source_set_event_handler(source, handler)
        }
    }

    /// Sets the cancellation handler block for the dispatch source.
    public func setCancelHandler(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], handler: DispatchSourceHandler?) {
        let source = _dispatchSource(self)
        if let handler = handler, !flags.isEmpty {
            _withFlagged(handler, flags) { ___alchemy_dispatch_source_set_cancel_handler(source, $0) }
        } else {
            __dispatch_source_set_cancel_handler(source, handler)
        }
    }

    /// Resumes the dispatch source.
    public func resume() { __dispatch_resume(_dispatchSource(self)) }

    /// Cancels the dispatch source: its event handler does not run again, and its cancellation handler runs.
    public func cancel() { __dispatch_source_cancel(_dispatchSource(self)) }
}

extension DispatchSourceFileSystemObject {
    /// The events the file-system object source observed.
    public var data: DispatchSource.FileSystemEvent {
        return DispatchSource.FileSystemEvent(rawValue: __dispatch_source_get_data(_dispatchSource(self)))
    }
}
