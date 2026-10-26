#include "alchemy/darwin/appkit/status_items.hpp"

#include "alchemy/darwin/appkit/controls_internal.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Status items and sounds.
// - NSStatusBar (+systemStatusBar) makes NSStatusItems; each visible item is
//   an icon in the host's notification area (the window server's status
//   items). The icon is the item button's image (a template image drawn white
//   on a dark appearance, black on a light one), else its title drawn as
//   text; the tool tip is the button's tool tip, else its title. A click
//   (either button) opens the item's menu at the pointer when it has one;
//   otherwise a left click (and a right click when the button sends on
//   right-mouse-up) sends the button's action. The status bar's thickness
//   (22 points) is AppKit's menu bar height; items have no place in a menu
//   bar, so their length is kept for the getters. Custom item views
//   (-setView:) are not drawn into the notification area: an explicit stop.
// - NSSound plays files and data through AVAudioPlayer (the AVFoundation
//   provider; linear PCM WAVE, AIFF / AIFC and CAF): play, pause, resume,
//   stop, volume, current time, duration, loops, and the delegate's
//   -sound:didFinishPlaying:. +soundNamed: finds sounds registered by
//   -setName:, then the main bundle's .aiff / .aif / .wav / .caf resources,
//   then /System/Library/Sounds; the system sounds are not installed, so their
//   names answer nil. Pasteboard sounds are not read (an explicit stop).
namespace alchemy::gui::appkit {
namespace {

constexpr double status_bar_thickness = 22;   // AppKit's menu bar height (points)
constexpr std::uint64_t right_mouse_up_mask = 1ULL << 4, right_mouse_down_mask = 1ULL << 3;

struct StatusItemState final : ObjectPayload {
    Address bar{};                            // not retained (the bar holds its items)
    Address button{}, menu{}, autosave_name{};   // +1
    double length{-1};                        // NSVariableStatusItemLength
    bool visible{true};
    std::int64_t behavior{};
    StatusItemId id{};                        // the window server's item, 0 while not shown
};
struct StatusBarState final : ObjectPayload {
    std::vector<Address> items;               // +1
};
struct SoundState final : ObjectPayload {
    Address player{}, forwarder{}, name{}, device{}, mapping{};   // +1
    Address delegate{};                       // not retained
    bool loops{};
    bool paused{};
};

struct Registry {
    std::mutex mutex;
    std::map<StatusItemId, Address> items;    // shown items by window server id (not retained)
    std::map<Address, Address> buttons;       // button -> item
    std::map<std::string, Address> sounds;    // -setName: (not retained: names die with the sound)
};
Registry& registry() {
    static Registry value;
    return value;
}

} // namespace

// The icon and tool tip of `item` (its button's image or title) shown, updated or removed.
static void refresh_status_item(AppKit& kit, Address item) {
    const auto state = kit.objc.host_state<StatusItemState>(item);
    auto* server = window_server_or_null();
    if (!server) return;
    if (!state->visible) {
        if (const auto id = std::exchange(state->id, 0)) {
            server->remove_status_item(id);
            std::lock_guard lock(registry().mutex);
            registry().items.erase(id);
        }
        return;
    }
    const auto button = state->button;
    const auto picture = button ? kit.send(button, "image").x0 : 0;
    const auto title = button ? kit.send(button, "title").x0 : 0;
    const auto title_text = title ? kit.cf.utf8(title) : std::string();
    std::optional<Image> pixels;
    const bool dark = [&] {
        const auto appearance = kit.app ? kit.send(kit.app, "effectiveAppearance").x0 : 0;
        const auto name = appearance ? kit.send(appearance, "name").x0 : 0;
        return name && kit.cf.utf8(name).find("Dark") != std::string::npos;
    }();
    if (picture) {
        pixels = kit.image_pixels(picture);
        // Template images are masks: drawn in the appearance's text color.
        if (pixels && kit.send_bool(picture, "isTemplate"))
            for (std::size_t at = 0; at + 3 < pixels->rgba.size(); at += 4)
                std::fill_n(pixels->rgba.begin() + static_cast<std::ptrdiff_t>(at), 3, static_cast<std::uint8_t>(dark ? 255 : 0));
    }
    if (!pixels && !title_text.empty()) {
        // The title, drawn into a square image (its first characters).
        CallRegs size;
        size.set_d(0, 32);
        size.set_d(1, 32);
        const auto canvas = kit.send(kit.send(kit.image, "alloc").x0, "initWithSize:", size).x0;
        static_cast<void>(kit.send(canvas, "lockFocus"));
        CallRegs font;
        font.set_d(0, title_text.size() <= 2 ? 22 : 13);
        const auto attributes = kit.cf.dictionary(std::vector<std::pair<Address, Address>>{
            {kit.auto_string("NSFont"), kit.send(kit.font, "boldSystemFontOfSize:", font).x0},
            {kit.auto_string("NSColor"), kit.send(kit.color, dark ? "whiteColor" : "blackColor").x0}});
        CallRegs draw;
        draw.set_d(0, 1);
        draw.set_d(1, 4);
        draw.x[2] = attributes;
        static_cast<void>(kit.send(title, "drawAtPoint:withAttributes:", draw));
        kit.release(attributes);
        static_cast<void>(kit.send(canvas, "unlockFocus"));
        pixels = kit.image_pixels(canvas);
        kit.release(canvas);
    }
    if (!pixels) {
        Image blank;
        blank.width = blank.height = 16;
        blank.rgba.assign(16 * 16 * 4, 0);
        pixels = blank;
    }
    const auto tip_object = button ? kit.send(button, "toolTip").x0 : 0;
    const auto tip = tip_object ? kit.cf.utf8(tip_object) : title_text;
    if (state->id) {
        server->update_status_item(state->id, *pixels, tip);
    } else {
        state->id = server->create_status_item(*pixels, tip);
        std::lock_guard lock(registry().mutex);
        registry().items[state->id] = item;
    }
}

void status_item_clicked(AppKit& kit, const Event& event) {
    Address item{};
    {
        std::lock_guard lock(registry().mutex);
        if (const auto found = registry().items.find(event.status_item); found != registry().items.end()) item = found->second;
    }
    if (!item) return;
    const auto state = kit.objc.host_state<StatusItemState>(item);
    if (state->menu) {
        CallRegs registers;
        registers.x[2] = 0;
        put_point(registers, 0, event.screen_location);
        registers.x[3] = 0;
        static_cast<void>(kit.send(state->menu, "popUpMenuPositioningItem:atLocation:inView:", registers));
        return;
    }
    const auto button = state->button;
    if (!button) return;
    const auto cell = kit.send(button, "cell").x0;
    const auto mask = cell ? cell_state(kit, cell)->send_action_mask : 0;
    if (event.button_number == 1 && !(mask & (right_mouse_up_mask | right_mouse_down_mask))) return;
    const auto action = kit.send(button, "action").x0;
    if (action) static_cast<void>(kit.send_action(action, kit.send(button, "target").x0, button));
}

void install_status_items(AppKit& kit) {
    auto* k = &kit;
    const auto bar_class = kit.declare_class("NSStatusBar", "NSObject");
    const auto item_class = kit.declare_class("NSStatusItem", "NSObject");
    const auto button_class = kit.declare_class("NSStatusBarButton", "NSButton");
    const auto item_of = [k](Address value) { return k->objc.host_state<StatusItemState>(value); };

    // ---- NSStatusBar ----
    static Address system_bar{};
    kit.class_method(bar_class, "systemStatusBar", [k, bar_class](CpuState& s, GuestMemory&) {
        if (!system_bar) {
            system_bar = k->send(k->send(bar_class, "alloc").x0, "init").x0;
            if (const auto record = k->process.objects().find(system_bar)) record->immortal = true;
        }
        s.x[0] = system_bar;
    });
    kit.method(bar_class, "statusItemWithLength:", [k, item_class, button_class, item_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto item = k->send(k->send(item_class, "alloc").x0, "init").x0;
        const auto state = item_of(item);
        state->bar = self;
        state->length = d(s, 0);
        CallRegs frame;
        put_rect(frame, 0, {{0, 0}, {state->length > 0 ? state->length : status_bar_thickness, status_bar_thickness}});
        state->button = k->send(k->send(button_class, "alloc").x0, "initWithFrame:", frame).x0;
        static_cast<void>(k->send_id(state->button, "setBordered:", {0}));
        {
            std::lock_guard lock(registry().mutex);
            registry().buttons[state->button] = item;
        }
        k->objc.host_state<StatusBarState>(self)->items.push_back(item);   // the bar's reference
        refresh_status_item(*k, item);
        s.x[0] = item;
    }, "@@:d");
    kit.method(bar_class, "removeStatusItem:", [k, item_of](CpuState& s, GuestMemory&) {
        const auto item = s.x[2];
        if (!item) return;
        auto& items = k->objc.host_state<StatusBarState>(s.x[0])->items;
        const auto found = std::find(items.begin(), items.end(), item);
        if (found == items.end()) return;
        items.erase(found);
        const auto state = item_of(item);
        state->visible = false;
        refresh_status_item(*k, item);
        k->release(item);
    }, "v@:@");
    kit.method(bar_class, "thickness", [](CpuState& s, GuestMemory&) { set_d(s, 0, status_bar_thickness); }, "d@:");
    kit.method(bar_class, "isVertical", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");

    // ---- NSStatusItem ----
    kit.method(item_class, "dealloc", [k, item_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<StatusItemState>(s.x[0])) {
            if (const auto id = std::exchange(state->id, 0)) {
                if (auto* server = window_server_or_null()) server->remove_status_item(id);
                std::lock_guard lock(registry().mutex);
                registry().items.erase(id);
            }
            {
                std::lock_guard lock(registry().mutex);
                registry().buttons.erase(state->button);
            }
            for (auto* slot : {&state->button, &state->menu, &state->autosave_name}) k->release(std::exchange(*slot, 0));
        }
        static_cast<void>(k->objc.send_super(s.x[0], item_class, k->sel("dealloc")));
    }, "v@:");
    kit.method(item_class, "statusBar", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->bar; });
    kit.method(item_class, "button", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->button; });
    kit.method(item_class, "length", [item_of](CpuState& s, GuestMemory&) { set_d(s, 0, item_of(s.x[0])->length); }, "d@:");
    kit.method(item_class, "setLength:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->length = d(s, 0); }, "v@:d");
    kit.method(item_class, "menu", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->menu; });
    kit.method(item_class, "setMenu:", [k, item_of](CpuState& s, GuestMemory&) { k->assign(item_of(s.x[0])->menu, s.x[2]); }, "v@:@");
    kit.method(item_class, "isVisible", [item_of](CpuState& s, GuestMemory&) { return_bool(s, item_of(s.x[0])->visible); }, "B@:");
    kit.method(item_class, "setVisible:", [k, item_of](CpuState& s, GuestMemory&) {
        item_of(s.x[0])->visible = bool_arg(s, 2);
        refresh_status_item(*k, s.x[0]);
    }, "v@:B");
    kit.method(item_class, "behavior", [item_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(item_of(s.x[0])->behavior); }, "Q@:");
    kit.method(item_class, "setBehavior:", [item_of](CpuState& s, GuestMemory&) { item_of(s.x[0])->behavior = static_cast<std::int64_t>(s.x[2]); }, "v@:Q");
    kit.method(item_class, "autosaveName", [item_of](CpuState& s, GuestMemory&) { s.x[0] = item_of(s.x[0])->autosave_name; });
    kit.method(item_class, "setAutosaveName:", [k, item_of](CpuState& s, GuestMemory&) {
        k->release(std::exchange(item_of(s.x[0])->autosave_name, s.x[2] ? k->cf.copy(s.x[2]) : 0));
    }, "v@:@");
    // The macOS 10.10 deprecated accessors: the button's.
    for (const auto& [selector, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"title", "@@:"}, {"setTitle:", "v@:@"}, {"attributedTitle", "@@:"}, {"setAttributedTitle:", "v@:@"},
             {"image", "@@:"}, {"setImage:", "v@:@"}, {"alternateImage", "@@:"}, {"setAlternateImage:", "v@:@"},
             {"toolTip", "@@:"}, {"setToolTip:", "v@:@"}, {"action", ":@:"}, {"setAction:", "v@::"},
             {"target", "@@:"}, {"setTarget:", "v@:@"}, {"isEnabled", "B@:"}, {"setEnabled:", "v@:B"},
             {"sendActionOn:", "q@:Q"}})
        kit.method(item_class, selector, [k, item_of, name = std::string(selector)](CpuState& s, GuestMemory&) {
            CallRegs registers;
            for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
            s.x[0] = k->send(item_of(s.x[0])->button, name, registers).x0;
        }, types);
    kit.method(item_class, "highlightMode", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    kit.method(item_class, "setHighlightMode:", [](CpuState&, GuestMemory&) {}, "v@:B");
    kit.method(item_class, "view", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    kit.method(item_class, "setView:", [](CpuState& s, GuestMemory&) {
        if (s.x[2]) unsupported("-[NSStatusItem setView:] (custom views are not drawn in the notification area)");
    }, "v@:@");
    kit.method(item_class, "popUpStatusItemMenu:", [k](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        CallRegs registers;
        put_point(registers, 0, require_window_server("-[NSStatusItem popUpStatusItemMenu:]").mouse_location());
        static_cast<void>(k->send(s.x[2], "popUpMenuPositioningItem:atLocation:inView:", registers));
    }, "v@:@");
    kit.method(item_class, "drawStatusBarBackgroundInRect:withHighlight:", [](CpuState&, GuestMemory&) {},
               "v@:{CGRect={CGPoint=dd}{CGSize=dd}}B");

    // ---- NSStatusBarButton: changes reach the notification area ----
    for (const auto& [selector, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"setImage:", "v@:@"}, {"setTitle:", "v@:@"}, {"setAttributedTitle:", "v@:@"}, {"setToolTip:", "v@:@"}})
        kit.method(button_class, selector, [k, button_class, name = std::string(selector)](CpuState& s, GuestMemory&) {
            CallRegs registers;
            registers.x[2] = s.x[2];
            static_cast<void>(k->objc.send_super(s.x[0], button_class, k->sel(name), registers));
            Address item{};
            {
                std::lock_guard lock(registry().mutex);
                if (const auto found = registry().buttons.find(s.x[0]); found != registry().buttons.end()) item = found->second;
            }
            if (item) refresh_status_item(*k, item);
        }, types);
    kit.method(button_class, "appearsDisabled", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    kit.method(button_class, "setAppearsDisabled:", [](CpuState&, GuestMemory&) {}, "v@:B");

    // ---- NSSound ----
    const auto sound_class = kit.declare_class("NSSound", "NSObject");
    const auto forwarder_class = kit.declare_class("_AlchemySoundForwarder", "NSObject");
    const auto sound_of = [k](Address value) { return k->objc.host_state<SoundState>(value); };
    // The AVAudioPlayer delegate: -audioPlayerDidFinishPlaying:successfully: -> -sound:didFinishPlaying:.
    struct ForwarderState final : ObjectPayload { Address sound{}; };
    kit.method(forwarder_class, "audioPlayerDidFinishPlaying:successfully:", [k, sound_of](CpuState& s, GuestMemory&) {
        const auto sound = k->objc.host_state<ForwarderState>(s.x[0])->sound;
        if (!sound) return;
        const auto delegate = sound_of(sound)->delegate;
        if (delegate && k->responds(delegate, "sound:didFinishPlaying:"))
            static_cast<void>(k->send_id(delegate, "sound:didFinishPlaying:", {sound, s.x[3] & 0xff}));
    }, "v@:@B");
    // Gives `self` an AVAudioPlayer made by `make` (nil when it cannot load); answers self or nil.
    const auto adopt_player = [k, sound_of, forwarder_class](Address self, Address player) -> Address {
        if (!player) { k->release(self); return 0; }
        const auto state = sound_of(self);
        state->player = player;
        state->forwarder = k->send(k->send(forwarder_class, "alloc").x0, "init").x0;
        k->objc.host_state<ForwarderState>(state->forwarder)->sound = self;
        static_cast<void>(k->send_id(player, "setDelegate:", {state->forwarder}));
        return self;
    };
    const auto player_class = [k] {
        const auto cls = k->objc.find_class("AVAudioPlayer");
        if (!cls) unsupported("NSSound without AVFoundation's AVAudioPlayer");
        return cls;
    };
    kit.method(sound_class, "initWithContentsOfURL:byReference:", [k, sound_class, adopt_player, player_class](CpuState& s, GuestMemory&) {
        const auto self = k->objc.send_super(s.x[0], sound_class, k->sel("init")).x0;
        const auto player = s.x[2] ? k->send_id(k->send(player_class(), "alloc").x0, "initWithContentsOfURL:error:", {s.x[2], 0}) : 0;
        s.x[0] = adopt_player(self, player);
    }, "@@:@B");
    kit.method(sound_class, "initWithContentsOfFile:byReference:", [k](CpuState& s, GuestMemory&) {
        const auto url = s.x[2] ? k->send_id(k->objc.find_class("NSURL"), "fileURLWithPath:", {s.x[2]}) : 0;
        s.x[0] = k->send_id(s.x[0], "initWithContentsOfURL:byReference:", {url, s.x[3]});
    }, "@@:@B");
    kit.method(sound_class, "initWithData:", [k, sound_class, adopt_player, player_class](CpuState& s, GuestMemory&) {
        const auto self = k->objc.send_super(s.x[0], sound_class, k->sel("init")).x0;
        const auto player = s.x[2] ? k->send_id(k->send(player_class(), "alloc").x0, "initWithData:error:", {s.x[2], 0}) : 0;
        s.x[0] = adopt_player(self, player);
    });
    kit.method(sound_class, "initWithPasteboard:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSSound initWithPasteboard:] (pasteboard sounds are not read)");
    });
    kit.class_method(sound_class, "canInitWithPasteboard:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@");
    kit.class_method(sound_class, "soundUnfilteredTypes", [k](CpuState& s, GuestMemory&) {
        s.x[0] = k->auto_array({k->auto_string("com.microsoft.waveform-audio"), k->auto_string("public.aiff-audio"),
                                k->auto_string("public.aifc-audio"), k->auto_string("com.apple.coreaudio-format")});
    });
    kit.method(sound_class, "dealloc", [k, sound_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<SoundState>(s.x[0])) {
            if (state->player) static_cast<void>(k->send(state->player, "stop"));
            if (state->forwarder) k->objc.host_state<ForwarderState>(state->forwarder)->sound = 0;
            if (state->name) {
                std::lock_guard lock(registry().mutex);
                registry().sounds.erase(k->cf.utf8(state->name));
            }
            for (auto* slot : {&state->player, &state->forwarder, &state->name, &state->device, &state->mapping})
                k->release(std::exchange(*slot, 0));
        }
        static_cast<void>(k->objc.send_super(s.x[0], sound_class, k->sel("dealloc")));
    }, "v@:");
    kit.class_method(sound_class, "soundNamed:", [k](CpuState& s, GuestMemory&) {
        const auto name = s.x[2] ? k->cf.utf8(s.x[2]) : std::string();
        if (name.empty()) { s.x[0] = 0; return; }
        {
            std::lock_guard lock(registry().mutex);
            if (const auto found = registry().sounds.find(name); found != registry().sounds.end()) { s.x[0] = found->second; return; }
        }
        Address path{};
        const auto bundle = k->send(k->objc.find_class("NSBundle"), "mainBundle").x0;
        for (const auto* type : {"aiff", "aif", "wav", "caf"}) {
            if (bundle && !path) path = k->send_id(bundle, "pathForResource:ofType:", {s.x[2], k->auto_string(type)});
            if (!path) {
                const auto system = k->auto_string("/System/Library/Sounds/" + name + "." + type);
                const auto manager = k->send(k->objc.find_class("NSFileManager"), "defaultManager").x0;
                if (manager && k->send_bool(manager, "fileExistsAtPath:", {system})) path = system;
            }
        }
        if (!path) { s.x[0] = 0; return; }
        const auto made = k->send_id(k->send(k->objc.find_class("NSSound"), "alloc").x0, "initWithContentsOfFile:byReference:", {path, 1});
        if (!made) { s.x[0] = 0; return; }
        static_cast<void>(k->send_id(made, "setName:", {s.x[2]}));
        s.x[0] = k->autorelease(made);
    });
    kit.method(sound_class, "name", [sound_of](CpuState& s, GuestMemory&) { s.x[0] = sound_of(s.x[0])->name; });
    kit.method(sound_class, "setName:", [k, sound_of](CpuState& s, GuestMemory&) {
        const auto state = sound_of(s.x[0]);
        const auto wanted = s.x[2] ? k->cf.utf8(s.x[2]) : std::string();
        std::lock_guard lock(registry().mutex);
        if (!wanted.empty() && registry().sounds.contains(wanted) && registry().sounds[wanted] != s.x[0]) { s.x[0] = 0; return; }
        if (state->name) registry().sounds.erase(k->cf.utf8(state->name));
        k->release(std::exchange(state->name, s.x[2] ? k->cf.copy(s.x[2]) : 0));
        if (!wanted.empty()) registry().sounds[wanted] = s.x[0];
        s.x[0] = 1;
    }, "B@:@");
    kit.method(sound_class, "delegate", [sound_of](CpuState& s, GuestMemory&) { s.x[0] = sound_of(s.x[0])->delegate; });
    kit.method(sound_class, "setDelegate:", [sound_of](CpuState& s, GuestMemory&) { sound_of(s.x[0])->delegate = s.x[2]; }, "v@:@");
    // -play answers NO when the sound is playing already.
    kit.method(sound_class, "play", [k, sound_of](CpuState& s, GuestMemory&) {
        const auto state = sound_of(s.x[0]);
        if (k->send_bool(state->player, "isPlaying")) { s.x[0] = 0; return; }
        static_cast<void>(k->send_id(state->player, "setNumberOfLoops:", {state->loops ? ~0ULL : 0ULL}));
        state->paused = false;
        s.x[0] = k->send_bool(state->player, "play") ? 1 : 0;
    }, "B@:");
    kit.method(sound_class, "pause", [k, sound_of](CpuState& s, GuestMemory&) {
        const auto state = sound_of(s.x[0]);
        if (!k->send_bool(state->player, "isPlaying")) { s.x[0] = 0; return; }
        static_cast<void>(k->send(state->player, "pause"));
        state->paused = true;
        s.x[0] = 1;
    }, "B@:");
    kit.method(sound_class, "resume", [k, sound_of](CpuState& s, GuestMemory&) {
        const auto state = sound_of(s.x[0]);
        if (!state->paused) { s.x[0] = 0; return; }
        state->paused = false;
        s.x[0] = k->send_bool(state->player, "play") ? 1 : 0;
    }, "B@:");
    kit.method(sound_class, "stop", [k, sound_of](CpuState& s, GuestMemory&) {
        const auto state = sound_of(s.x[0]);
        const bool was = k->send_bool(state->player, "isPlaying") || state->paused;
        static_cast<void>(k->send(state->player, "stop"));
        CallRegs zero;
        zero.set_d(0, 0);
        static_cast<void>(k->send(state->player, "setCurrentTime:", zero));
        state->paused = false;
        s.x[0] = was ? 1 : 0;
    }, "B@:");
    kit.method(sound_class, "isPlaying", [k, sound_of](CpuState& s, GuestMemory&) { return_bool(s, k->send_bool(sound_of(s.x[0])->player, "isPlaying")); }, "B@:");
    kit.method(sound_class, "volume", [k, sound_of](CpuState& s, GuestMemory&) { set_f(s, 0, k->send(sound_of(s.x[0])->player, "volume").s(0)); }, "f@:");
    kit.method(sound_class, "setVolume:", [k, sound_of](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.v[0] = s.v[0];
        static_cast<void>(k->send(sound_of(s.x[0])->player, "setVolume:", registers));
    }, "v@:f");
    kit.method(sound_class, "duration", [k, sound_of](CpuState& s, GuestMemory&) { set_d(s, 0, k->send(sound_of(s.x[0])->player, "duration").d(0)); }, "d@:");
    kit.method(sound_class, "currentTime", [k, sound_of](CpuState& s, GuestMemory&) { set_d(s, 0, k->send(sound_of(s.x[0])->player, "currentTime").d(0)); }, "d@:");
    kit.method(sound_class, "setCurrentTime:", [k, sound_of](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.v[0] = s.v[0];
        static_cast<void>(k->send(sound_of(s.x[0])->player, "setCurrentTime:", registers));
    }, "v@:d");
    kit.method(sound_class, "loops", [sound_of](CpuState& s, GuestMemory&) { return_bool(s, sound_of(s.x[0])->loops); }, "B@:");
    kit.method(sound_class, "setLoops:", [sound_of](CpuState& s, GuestMemory&) { sound_of(s.x[0])->loops = bool_arg(s, 2); }, "v@:B");
    // Kept for the getters: sounds play on the default output device, every channel.
    kit.method(sound_class, "playbackDeviceIdentifier", [sound_of](CpuState& s, GuestMemory&) { s.x[0] = sound_of(s.x[0])->device; });
    kit.method(sound_class, "setPlaybackDeviceIdentifier:", [k, sound_of](CpuState& s, GuestMemory&) {
        k->release(std::exchange(sound_of(s.x[0])->device, s.x[2] ? k->cf.copy(s.x[2]) : 0));
    }, "v@:@");
    kit.method(sound_class, "channelMapping", [sound_of](CpuState& s, GuestMemory&) { s.x[0] = sound_of(s.x[0])->mapping; });
    kit.method(sound_class, "setChannelMapping:", [k, sound_of](CpuState& s, GuestMemory&) {
        k->release(std::exchange(sound_of(s.x[0])->mapping, s.x[2] ? k->cf.copy(s.x[2]) : 0));
    }, "v@:@");
}

} // namespace alchemy::gui::appkit
