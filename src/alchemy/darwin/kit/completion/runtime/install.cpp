#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/dispatch/install.hpp"
#include "alchemy/darwin/libsystem/install.hpp"
#include "alchemy/darwin/corefoundation/install.hpp"
#include "alchemy/darwin/foundation/install.hpp"

// QLab gap group G2 (runtime): Foundation runtime and system (exceptions, locks, invocations, operations, notification queues, undo, KVO/bindings controllers, tasks/pipes/file handles, scripting, XPC, os_log store, dispatch objects).
// Owner and scope: docs/ALCHEMY_QLAB_GAPS.md, group G2. Each provider file in
// this directory adds one install_* function called from here; cmake/darwin/kit_completion_runtime.cmake
// lists the sources and the group's tests.
//   common.cpp            shared helpers (guest strings, NSError, blocks)
//   dispatch_objects.cpp  OS_dispatch_* classes (dispatch objects as Objective-C objects), dispatch I/O channels
//   exceptions.cpp        NSException and the exception name exports
//   threads.cpp           NSThread sleeping and call stacks, NSLock/NSRecursiveLock/NSCondition/NSConditionLock
//   invocation.cpp        NSMethodSignature, NSInvocation (core: invocation_core.cpp)
//   undo.cpp              NSUndoManager and its invocation proxy (core: undo_core.cpp)
//   kvo.cpp               key-value observing (registration, will/did, NSKVONotifying_ subclasses, dependent keys)
//   bindings.cpp          Cocoa bindings, NSObjectController, NSArrayController, _NSManagedProxy
//   kvc_collections.cpp   -mutableArrayValueForKey: proxies
//   notification_queue.cpp NSNotificationQueue, NSProcessInfo automatic termination
//   process_io.cpp        NSFileHandle, NSPipe, NSTask, NSStream/NSInputStream
//   files.cpp             NSFileManager additions, NSDirectoryEnumerator
//   bundles.cpp           NSBundle additions, CFBundleCopyInfoDictionaryInDirectory
//   system_c.cpp          libSystem/CoreFoundation/Foundation/ServiceManagement C interfaces (OSAtomic, asl, termios, CFMessagePort, ...)
//   log_store.cpp         the process's os_log entries and message composition (core)
//   oslog_store.cpp       OSLogStore, OSLogPosition, OSLogEntry, OSLogEntryLog
//   sdef.cpp              scripting definition (.sdef) parsing (core)
//   scripting.cpp         Cocoa scripting classes, specifiers and commands; NSAppleScript; NSAppleEventDescriptor
//   xpc.cpp               NSXPCInterface, NSXPCConnection, NSXPCListener (no XPC services: connecting fails explicitly)
//   cf_extras.cpp         CFBundleGetVersionNumber, CFFileDescriptor, CF class identities, _c_locale, fts, warn/err
namespace alchemy::gui::completion_kit {

void install_group_runtime(Support& support) {
    alchemy::dispatch::install_dispatch_objects(support);
    gui::install_foundation_exceptions(support);
    alchemy::libsystem::install_threads(support);
    alchemy::libsystem::install_locks(support);
    gui::install_foundation_invocations_undo_kvo(support);
    install_bindings(support);
    gui::install_foundation_kvc_collections_and_queues(support);
    alchemy::libsystem::install_process_io(support);
    gui::install_foundation_files_and_bundles(support);
    alchemy::libsystem::install_system_functions(support);
    alchemy::libsystem::install_os_log_store(support);
    gui::install_foundation_scripting(support);
    alchemy::libsystem::install_xpc(support);
    gui::install_corefoundation_file_descriptors(support);
}

} // namespace alchemy::gui::completion_kit
