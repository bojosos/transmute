#pragma once

// Key-value observing (kvo.cpp) for the other providers of this directory
// (bindings, controllers) and kit/completion/kvc.cpp. Not a contract header.

#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <functional>

namespace alchemy::gui::completion_kit::kvo {

inline constexpr std::uint64_t option_new = 1, option_old = 2, option_initial = 4, option_prior = 8;
inline constexpr std::uint64_t kind_setting = 1, kind_insertion = 2, kind_removal = 3, kind_replacement = 4;

// An internal observation: `callback(root, change)` runs where an observer's
// -observeValueForKeyPath:ofObject:change:context: would (change is the +0
// change dictionary). Returns the observation's id.
using Callback = std::function<void(Address root, Address change)>;
std::uint64_t observe(Support& support, Address object, std::string key_path, std::uint64_t options, Callback callback);
void unobserve(Support& support, std::uint64_t id);

// True when `object` has observers of `key` whose change notifications are
// automatic (+automaticallyNotifiesObserversForKey: YES): KVC writes that
// bypass accessors (instance variables) post will/didChangeValueForKey:.
[[nodiscard]] bool notifies_automatically(Support& support, Address object, std::string_view key);

// Runs `cleanup(object)` after the object's -dealloc, when its memory is
// released (object-store objects; others are ignored). Once per object and
// owner tag.
void on_dealloc(Support& support, Address object, std::string_view tag, std::function<void(Address)> cleanup);

// Called when an observation path starts observing `key` of an instance of
// `cls` (proxies forward observation to the objects behind them).
using LinkHook = std::function<void(Address object, const std::string& key)>;
void set_link_hook(Support& support, Address cls, LinkHook hook);

} // namespace alchemy::gui::completion_kit::kvo
