#pragma once

// QLab 5 support (owner: mac/qlab, src/alchemy/darwin/kit/completion/): the macOS
// interfaces QLab 5.6.3 and its bundled frameworks (SnoizeMIDI, Syphon,
// Sparkle) reach at launch that no other GUI provider implements
// (docs/ALCHEMY_QLAB_AUDIT.md). Installed last by install_completion_kit,
// after the support kit's gap fillers, so it can complete the classes they and
// the other providers declare.
//   foundation.cpp   NSDistributedNotificationCenter (in-process delivery), NSKeyedArchiver class names
//   oslog.cpp        os_log objects as Objective-C OS_os_log instances
//   audio.cpp        AudioToolbox CAClock (a software media clock)
//   kvc.cpp          NSKeyValueCoding on NSObject and NSArray (when Foundation lacks it)
//   formatters.cpp   NSFormatter / NSNumberFormatter / NSDateFormatter (cores: number_format.cpp, date_format.cpp)
//   collections.cpp  NSNull (kCFNull), NSOrderedSet / NSMutableOrderedSet
//   json_serialization.cpp NSJSONSerialization (json.cpp: parser and writer)
//   constants.cpp    missing Foundation/AppKit string constants and NSRect functions
//   file_attributes.cpp NSFileManager item attributes from the guest stat
//   immutable_copies.cpp NSCopying of immutable value classes (copy = retain)
//   disk_arbitration    DiskArbitration over the guest volumes (darwin/diskarbitration/sessions.cpp)
//   media_timing.cpp CAMediaTimingFunction and its name constants
//   animator.cpp     -animator (the receiver: no animation engine), -animations
//   keyed_archive.cpp NSKeyedArchiver / NSKeyedUnarchiver (binary or XML plist archives)
//   data_files.cpp   NSData file reading and writing through the guest libSystem
//   install.cpp      install_completion_kit
// The helpers are the support kit's (support_kit::Support).

#include "alchemy/darwin/kit/support/support_kit.hpp"

namespace alchemy::gui::completion_kit {

using support_kit::Support;
using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
using Handler = transmute::GuestCallHandler;

inline constexpr std::string_view foundation_library = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
inline constexpr std::string_view appkit_library = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";

[[noreturn]] void fail(std::string_view why);          // "Alchemy completion kit: <why>"
[[noreturn]] void unsupported(std::string_view what);  // "unsupported: Alchemy completion kit: <what>"

void install_foundation(Support& support);
void install_os_log(Support& support);
void install_audio(Support& support);
void install_key_value_coding(Support& support);
void install_formatters(Support& support);
void install_collections(Support& support);
void install_json_serialization(Support& support);
void install_constants(Support& support);
void install_file_attributes(Support& support);
void install_immutable_copies(Support& support);
void install_disk_arbitration(Support& support);
void install_media_timing(Support& support);
void install_animator(Support& support);
void install_keyed_archive(Support& support);
void install_data_files(Support& support);
// QLab gap groups (docs/ALCHEMY_QLAB_GAPS.md), one directory each.
void install_group_data(Support& support);   // G1: data/install.cpp
void install_group_runtime(Support& support);   // G2: runtime/install.cpp
void install_group_controls(Support& support);   // G3: controls/install.cpp
void install_group_windows(Support& support);   // G4: windows/install.cpp
void install_group_media(Support& support);   // G5: media/install.cpp
void install_group_graphics(Support& support);   // G6: graphics/install.cpp

} // namespace alchemy::gui::completion_kit
