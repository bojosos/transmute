#pragma once

// QuartzCore <-> AppKit contract (A7 QuartzCore <-> A4 AppKit).
//
// A CAMetalLayer presents to the window that shows it. AppKit decides that
// (the layer of a layer-backed or layer-hosting NSView that is in a window's
// view hierarchy) and tells QuartzCore with set_layer_host whenever the
// answer or its geometry changes:
// - -[NSView setLayer:] / -setWantsLayer: on a view in a window, and
//   -viewDidMoveToWindow (the view entered a window): the host;
// - view frame changes, window resize, backing-scale (DPI) changes: the new
//   frame and scale (QuartzCore recreates the swapchain itself when the
//   window's client size changes; the frame decides where the drawable is
//   drawn and is cheap to update every time);
// - removal from the window, -setLayer:nil, window close: std::nullopt.
// AppKit also keeps the layer's own geometry current like macOS does for a
// view's backing layer (objc.send setFrame:/setContentsScale:, which guest
// subclasses may observe); set_layer_host never changes layer properties.
//
// A CAMetalLayer without a host fails explicitly in -nextDrawable (headless
// processes never reach presentation).
//
// Threading: main thread (where AppKit runs); the presenting side reads the
// host under a lock from any thread.

#include "alchemy/gui/window_server.hpp"
#include "alchemy/objc_runtime.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <source_location>
#include <string_view>

namespace alchemy::gui {
struct InstallContext;
}
namespace alchemy::gui::quartzcore::tree {
struct Layer;
}

namespace alchemy::gui::quartzcore {

struct LayerHost {
    WindowId window{};          // window-server window whose content area shows the layer
    Rect frame;                 // the layer's frame in that content area: points, bottom-left origin
    double backing_scale{1};    // the window's backing scale factor
    // The content HWND. Null: window_server().native_handle(window), resolved
    // when the layer first presents (unit tests pass their own window).
    void* native_window{};
};

void set_layer_host(ObjcRuntime& objc, std::uint64_t layer, std::optional<LayerHost> host);
[[nodiscard]] std::optional<LayerHost> layer_host(ObjcRuntime& objc, std::uint64_t layer);

// True when `object` is an instance of the host CAMetalLayer class or a
// guest subclass of it.
[[nodiscard]] bool is_metal_layer(ObjcRuntime& objc, std::uint64_t object);

// Told after -[CALayer setContents:] stores a layer's contents, with the
// argument it was given, and with 0 after a layer that had contents was
// deallocated (layer_contents.cpp holds the window server's use of shown
// IOSurfaces this way). One observer per `domain`, for the QuartzCore of
// this process; the registration audit lists each one as an extension.
using ContentsObserver = std::function<void(std::uint64_t layer, std::uint64_t contents)>;
void observe_layer_contents(std::string_view domain, ContentsObserver observer,
                            const std::source_location& where = std::source_location::current());

// Composition (layer_contents.cpp, layer_tree.hpp). QuartzCore tells the
// change observers, on the changing thread and outside its locks, after every
// change to how a layer looks: geometry (bounds, position, anchorPoint,
// frame, zPosition, transforms, contentsScale), hierarchy (the superlayer of
// an added or removed layer), appearance (hidden, opaque, masksToBounds,
// geometryFlipped, opacity, backgroundColor, contents, contentsGravity) and
// -setContentsChanged. The commit observers are told when the outermost
// explicit CATransaction of a thread commits, and on +flush outside one.
// One observer of each kind per `domain`; the audit lists them as extensions.
using ChangeObserver = std::function<void(std::uint64_t layer)>;
using CommitObserver = std::function<void()>;
void observe_layer_changes(std::string_view domain, ChangeObserver observer,
                           const std::source_location& where = std::source_location::current());
void observe_transaction_commits(std::string_view domain, CommitObserver observer,
                                 const std::source_location& where = std::source_location::current());
// The calling thread is inside +[CATransaction begin] ... +commit.
[[nodiscard]] bool in_explicit_transaction() noexcept;
// The topmost superlayer of `layer` (the layer itself without one).
[[nodiscard]] std::uint64_t root_layer(ObjcRuntime& objc, std::uint64_t layer);
// `object` is a CALayer (or an instance of a subclass).
[[nodiscard]] bool is_layer(ObjcRuntime& objc, std::uint64_t object);
// The tree at `root` as it is now: every layer's model values, its
// backgroundColor's components and its contents object (not retained:
// tree::Layer::contents_object; contents stay unresolved). Main thread.
[[nodiscard]] tree::Layer snapshot_tree(ObjcRuntime& objc, std::uint64_t root);

// Creates the process's QuartzCore provider and registers its classes,
// methods and functions (install_quartzcore, install.cpp).
void install_provider(InstallContext& context);

} // namespace alchemy::gui::quartzcore
