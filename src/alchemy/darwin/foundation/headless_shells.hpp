#pragma once

// The headless shells of r2's frameworks: their own answers, in alchemy.exe,
// for names a GUI provider owns in alchemy-fx. Each <name>_headless.cpp is compiled into
// alchemy_core only (ALCHEMY_HEADLESS_ONLY_SOURCES); its <name>_fx.cpp twin is
// in the GUI source set (alchemy_core_fx only) and defines the same function
// empty, because there the GUI provider registers those names itself. Every
// name thus has one owner in each build. foundation_extra.cpp and
// darwin/headless_additions.cpp call these where they used to register the
// shells, so the install order is unchanged.
#include <cstdint>
#include <functional>

namespace alchemy {
class CoreFoundation;
class ObjcRuntime;
class ProcessContext;
} // namespace alchemy

namespace alchemy::foundation_shells {

// -[NSNotificationCenter addObserverForName:object:queue:usingBlock:], which
// fails explicitly. alchemy-fx: darwin/foundation/notification_blocks.cpp.
void install_notification_block_shell(ObjcRuntime& objc, std::uint64_t center_class);

// NSRunLoop's +currentRunLoop, +mainRunLoop (one per thread, from
// `run_loop_for(thread identity)`), and -run, -runMode:beforeDate:,
// -runUntilDate:, -currentMode, -limitDateForMode: and -addTimer:forMode: of a
// run loop without sources. alchemy-fx: darwin/runloop/timers.cpp.
void install_run_loop_shell(ObjcRuntime& objc, ProcessContext& process, std::uint64_t run_loop_class,
                            std::function<std::uint64_t(std::uint64_t)> run_loop_for);

// NSAttributedString over a plain string (no attributes are kept).
// alchemy-fx: darwin/coretext/attributed.cpp.
void install_attributed_string_shell(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf,
                                     std::uint64_t attributed_class);

// Called by darwin/headless_additions.cpp and its pieces (alchemy.exe's base shells):
// NSUserDefaults' methods over volatile domains (arguments, set*,
// registerDefaults:); alchemy-fx: darwin/corefoundation/cf_preferences.cpp.
// The class itself is registered in both builds
// (darwin/foundation/class_shells_and_events.cpp).
void install_user_defaults_shell(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf);
// CFDateFormatterCreate / CFDateFormatterGetFormat, explicit failures;
// alchemy-fx: darwin/corefoundation/cf_locale.cpp.
void install_cf_date_formatter_shell(ProcessContext& process);
// CFSocket*, explicit failures; alchemy-fx: darwin/network/socket.cpp.
void install_cfsocket_shell(ProcessContext& process);
// AuthorizationCreate / AuthorizationExecuteWithPrivileges; alchemy-fx:
// darwin/security/code_signing.cpp.
void install_authorization_shell(ProcessContext& process);

} // namespace alchemy::foundation_shells
