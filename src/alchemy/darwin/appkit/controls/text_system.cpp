#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <tuple>

// The TextKit 1 objects behind NSTextView (the inspectors' notes and the
// workspace's text views) and NSTextView's attribute, selection and typing
// API:
// * NSTextStorage, an NSMutableAttributedString (the CoreText layer's runs)
//   that reports its edits (-edited:range:changeInLength:, -processEditing)
//   to its delegate and its layout managers, which bring their text views
//   up to date;
// * NSLayoutManager (panels.cpp declares it): the storage, the containers,
//   temporary attributes, layout queries over the view's line layout;
// * NSTextContainer (text.cpp) additions and its nib decoding;
// * NSTextViewSharedData, the archived settings shared by a text view
//   network (flags, colors, selected / link / marked attributes, default
//   paragraph style), and NSTextView's nib decoding;
// * NSTextView: -textStorage / -layoutManager (made on demand, the view
//   owning the network), -initWithFrame:textContainer:, selected ranges,
//   typing / selected / marked / link attributes, default paragraph style,
//   insertion point color, text checking and inline prediction types,
//   -selectionRangeForProposedRange:granularity:, -complete: (the
//   delegate's completions in a menu), -insertLineBreak:;
// * NSFontManager's selected font and font family members, NSFontPanel's
//   -panelConvertFont: (no font panel UI: the font panel itself fails).
// The view's text is the storage's string; it draws in the attributes of
// the storage's first character (text.cpp draws one style).
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

// NSTextStorageEditActions.
constexpr std::uint64_t edited_attributes = 1, edited_characters = 2;

// NSTextStorage bookkeeping (the object's payload is the CoreText layer's
// attributed string state, so this lives beside it).
struct StorageInfo {
    std::vector<Address> layout_managers;    // +1
    Address delegate{};                      // not retained
    Address observer{};                      // -textStorageObserver (not retained)
    std::uint64_t mask{};
    std::size_t location{}, length{};        // the edited range (in the new text)
    std::int64_t delta{};
    std::int64_t depth{};                    // -beginEditing nesting
    bool edited{}, processing{};
};
std::map<Address, StorageInfo>& storages() {
    static std::map<Address, StorageInfo> table;
    return table;
}

std::shared_ptr<LayoutManagerState> layout_of(AppKit& kit, Address manager) { return kit.objc.host_state<LayoutManagerState>(manager); }
std::shared_ptr<ContainerState> container_of(AppKit& kit, Address container) { return kit.objc.host_state<ContainerState>(container); }

std::size_t length_of(AppKit& kit, Address string) { return string ? static_cast<std::size_t>(kit.send(string, "length").x0) : 0; }

// ---- temporary attributes: runs over the storage --------------------------------------
using Run = LayoutManagerState::Run;
void fit_runs(AppKit& kit, LayoutManagerState& state, std::size_t length) {
    std::size_t total = 0;
    for (const auto& run : state.temporary) total += run.length;
    if (total < length) state.temporary.push_back({length - total, 0});
    while (total > length && !state.temporary.empty()) {
        auto& last = state.temporary.back();
        const auto cut = std::min(last.length, total - length);
        last.length -= cut;
        total -= cut;
        if (last.length == 0) { kit.release(last.attributes); state.temporary.pop_back(); }
    }
}
std::size_t split_run(AppKit& kit, LayoutManagerState& state, std::size_t index) {
    std::size_t at = 0;
    for (std::size_t run = 0; run < state.temporary.size(); ++run) {
        if (at == index) return run;
        if (index < at + state.temporary[run].length) {
            const auto first = index - at;
            Run second{state.temporary[run].length - first, kit.retain(state.temporary[run].attributes)};
            state.temporary[run].length = first;
            state.temporary.insert(state.temporary.begin() + static_cast<std::ptrdiff_t>(run + 1), second);
            return run + 1;
        }
        at += state.temporary[run].length;
    }
    return state.temporary.size();
}
void coalesce_runs(AppKit& kit, LayoutManagerState& state) {
    std::vector<Run> runs;
    for (auto& run : state.temporary) {
        if (run.length == 0) { kit.release(run.attributes); continue; }
        if (!runs.empty() && (runs.back().attributes == run.attributes ||
                              (runs.back().attributes && run.attributes && kit.cf.equal(runs.back().attributes, run.attributes)))) {
            runs.back().length += run.length;
            kit.release(run.attributes);
            continue;
        }
        runs.push_back(run);
    }
    state.temporary = std::move(runs);
}
// Rewrites the dictionaries of [location, location + length).
void change_temporary(AppKit& kit, Address manager, std::size_t location, std::size_t length,
                      const std::function<Address(Address)>& change) {   // old (may be 0) -> new +1 (0: none)
    const auto state = layout_of(kit, manager);
    const auto total = length_of(kit, state->storage);
    if (location > total || length > total - location)
        unsupported("NSLayoutManager temporary attribute range beyond the text (NSRangeException)");
    fit_runs(kit, *state, total);
    const auto first = split_run(kit, *state, location);
    const auto last = split_run(kit, *state, location + length);
    for (auto run = first; run < last; ++run) {
        const auto replacement = change(state->temporary[run].attributes);
        kit.release(std::exchange(state->temporary[run].attributes, replacement));
    }
    coalesce_runs(kit, *state);
    for (const auto container : state->containers)
        if (const auto view = container_of(kit, container)->text_view) kit.invalidate_view(view);
}
Address dictionary_with(AppKit& kit, Address base, const std::vector<std::pair<Address, Address>>& set, const std::vector<Address>& remove) {
    auto entries = base ? kit.cf.dictionary_entries(base) : std::vector<std::pair<Address, Address>>{};
    const auto drop = [&](Address key) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& e) { return kit.cf.equal(e.first, key); }), entries.end());
    };
    for (const auto key : remove) drop(key);
    for (const auto& [key, value] : set) { drop(key); entries.emplace_back(key, value); }
    if (entries.empty()) return 0;
    return kit.cf.dictionary(entries);
}
// Shifts the runs for an edit of the storage (`location`, new `length`, `delta`).
void follow_edit(AppKit& kit, LayoutManagerState& state, std::size_t location, std::size_t length, std::int64_t delta) {
    if (state.temporary.empty()) return;
    const auto old_length = static_cast<std::int64_t>(length) - delta;
    const auto first = split_run(kit, state, location);
    const auto last = split_run(kit, state, location + static_cast<std::size_t>(std::max<std::int64_t>(0, old_length)));
    for (auto run = first; run < last; ++run) kit.release(std::exchange(state.temporary[run].attributes, 0));
    state.temporary.erase(state.temporary.begin() + static_cast<std::ptrdiff_t>(first), state.temporary.begin() + static_cast<std::ptrdiff_t>(last));
    if (length) state.temporary.insert(state.temporary.begin() + static_cast<std::ptrdiff_t>(first), Run{length, 0});
    coalesce_runs(kit, state);
}

// ---- storage <-> views --------------------------------------------------------------------
// The text views a storage shows in.
std::vector<Address> views_of_storage(AppKit& kit, Address storage) {
    std::vector<Address> views;
    const auto found = storages().find(storage);
    if (found == storages().end()) return views;
    for (const auto manager : found->second.layout_managers)
        for (const auto container : layout_of(kit, manager)->containers)
            if (const auto view = container_of(kit, container)->text_view) views.push_back(view);
    return views;
}
// The view's text and style from its storage.
void sync_view(AppKit& kit, Address view) {
    const auto state = text_state(kit, view);
    if (!state->storage || state->syncing) return;
    const auto string = kit.send_id(state->storage, "string");
    state->value = kit.string16(string);
    if (!state->value.empty()) {
        CallRegs registers;
        registers.x[2] = 0;
        registers.x[3] = 0;
        const auto attributes = kit.send(state->storage, "attributesAtIndex:effectiveRange:", registers).x0;
        if (attributes) {
            if (const auto font = dictionary_get(kit, attributes, "NSFont")) kit.assign(state->font, font);
            if (const auto color = dictionary_get(kit, attributes, "NSColor")) kit.assign(state->text_color, color);
        }
    }
    state->selection = std::min(state->selection, state->value.size());
    state->selection_length = std::min(state->selection_length, state->value.size() - state->selection);
    state->anchor = std::min(state->anchor, state->value.size());
    if (state->marked && state->marked->first + state->marked->second > state->value.size()) state->marked.reset();
    kit.invalidate_view(view);
    if (!state->field_editor && state->vertically_resizable) static_cast<void>(kit.send(view, "sizeToFit"));
}

void process_editing(AppKit& kit, Address storage) {
    auto& info = storages()[storage];
    if (!info.edited || info.processing) return;
    info.processing = true;
    try {
        const auto mask = info.mask;
        const auto location = info.location, length = info.length;
        const auto delta = info.delta;
        const auto notify = [&](const char* selector, const char* legacy, const char* name) {
            static_cast<void>(kit.post(name, storage));
            if (!info.delegate) return;
            if (kit.responds(info.delegate, selector)) {
                CallRegs registers;
                registers.x[2] = storage;
                registers.x[3] = mask;
                registers.x[4] = location;
                registers.x[5] = length;
                registers.x[6] = static_cast<Address>(delta);
                static_cast<void>(kit.send(info.delegate, selector, registers));
            } else if (kit.responds(info.delegate, legacy)) {
                static_cast<void>(kit.send_id(info.delegate, legacy, {kit.notification(name, storage)}));
            }
        };
        notify("textStorage:willProcessEditing:range:changeInLength:", "textStorageWillProcessEditing:", "NSTextStorageWillProcessEditingNotification");
        notify("textStorage:didProcessEditing:range:changeInLength:", "textStorageDidProcessEditing:", "NSTextStorageDidProcessEditingNotification");
        for (const auto manager : std::vector<Address>(info.layout_managers)) {
            CallRegs registers;
            registers.x[2] = storage;
            registers.x[3] = mask;
            registers.x[4] = location;
            registers.x[5] = length;
            registers.x[6] = static_cast<Address>(delta);
            registers.x[7] = location;   // invalidated range: the edited one
            static_cast<void>(kit.send(manager, "processEditingForTextStorage:edited:range:changeInLength:invalidatedRange:", registers));
        }
    } catch (...) {
        auto& again = storages()[storage];
        again.processing = false;
        again.edited = false;
        throw;
    }
    auto& after = storages()[storage];
    after.processing = false;
    after.edited = false;
    after.mask = 0;
    after.delta = 0;
}

void note_edit(AppKit& kit, Address storage, std::uint64_t mask, std::size_t location, std::size_t old_length, std::int64_t delta) {
    auto& info = storages()[storage];
    const auto new_length = static_cast<std::size_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(old_length) + delta));
    if (!info.edited) {
        info.location = location;
        info.length = new_length;
        info.delta = delta;
        info.mask = mask;
        info.edited = true;
    } else {
        // Union of the edited ranges (in the new text).
        const auto end = std::max(info.location + info.length, location + new_length);
        info.location = std::min(info.location, location);
        info.length = end - info.location;
        info.delta += delta;
        info.mask |= mask;
    }
    if (info.depth == 0) process_editing(kit, storage);
}

// Attributed-string archive contents (NSString, NSAttributes, NSAttributeInfo).
void decode_attributed(AppKit& kit, const Decoder& in, Address target) {
    const auto string = in.object("NSString");
    if (!string) return;
    const auto text = kit.cf.kind(string) == ObjectKind::string ? string : kit.send_id(string, "string");
    static_cast<void>(kit.send_id(target, "replaceCharactersInRange:withString:", {0, length_of(kit, target), text}));
    const auto total = length_of(kit, target);
    Address attributes{};
    try {
        attributes = in.object("NSAttributes");
    } catch (const std::exception& error) {
        // The nib loader makes archived NSParagraphStyle values nil (nib.cpp,
        // gap group G4), so an attribute dictionary holding one cannot be
        // built; the text then keeps the view's own style. Other failures stand.
        if (std::string_view(error.what()).find("null dictionary key or value") == std::string_view::npos) throw;
        note_trace("archived text attributes hold a value the nib loader leaves nil; the text keeps the view's style");
        return;
    }
    if (!attributes || !total) return;
    if (kit.cf.kind(attributes) == ObjectKind::dictionary) {
        static_cast<void>(kit.send_id(target, "setAttributes:range:", {attributes, 0, total}));
        return;
    }
    // Several runs: NSAttributeInfo is (run length, dictionary index) pairs as unsigned LEB128.
    const auto dictionaries = kit.items(attributes);
    const auto info = in.object("NSAttributeInfo");
    if (!info) unsupported("archived attributed string with several attribute runs but no NSAttributeInfo");
    const auto bytes = kit.data_bytes(info);
    std::size_t at = 0, position = 0;
    const auto next = [&]() -> std::uint64_t {
        std::uint64_t value = 0;
        for (unsigned shift = 0; at < bytes.size(); shift += 7) {
            const auto byte = bytes[at++];
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80)) return value;
        }
        fail("truncated NSAttributeInfo");
    };
    while (at < bytes.size() && position < total) {
        const auto length = next();
        const auto index = next();
        if (index >= dictionaries.size()) fail("NSAttributeInfo names a missing attribute dictionary");
        const auto run = std::min<std::uint64_t>(length, total - position);
        static_cast<void>(kit.send_id(target, "setAttributes:range:", {dictionaries[index], position, run}));
        position += run;
    }
}

// The word or paragraph around a character (selectionRangeForProposedRange:granularity:).
bool word_unit(char16_t c) { return c == u'_' || c == u'\'' || (c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') || c >= 0xc0; }
std::pair<std::size_t, std::size_t> granular_range(const std::u16string& text, std::size_t location, std::size_t length, std::uint64_t granularity) {
    location = std::min(location, text.size());
    length = std::min(length, text.size() - location);
    if (granularity == 0) return {location, length};
    auto start = location, end = location + length;
    if (granularity == 1) {   // NSSelectByWord
        if (start < text.size() && word_unit(text[start])) while (start > 0 && word_unit(text[start - 1])) --start;
        else if (start == text.size() && start > 0 && word_unit(text[start - 1])) while (start > 0 && word_unit(text[start - 1])) --start;
        if (end == start && end < text.size()) {
            if (word_unit(text[end])) while (end < text.size() && word_unit(text[end])) ++end;
            else ++end;
        } else {
            while (end > 0 && end < text.size() && word_unit(text[end - 1]) && word_unit(text[end])) ++end;
        }
        return {start, end - start};
    }
    // NSSelectByParagraph: to the paragraph separators, including the trailing one.
    const auto separator = [](char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2029; };
    while (start > 0 && !separator(text[start - 1])) --start;
    while (end < text.size() && !separator(text[end])) ++end;
    if (end < text.size()) ++end;
    return {start, end - start};
}

} // namespace

void install_text_system(AppKit& kit) {
    auto* const k = &kit;
    // NSTextStorage is an NSMutableAttributedString of the CoreText layer
    // (installed before this group in install_darwin_gui).
    if (!kit.objc.find_class("NSMutableAttributedString"))
        fail("NSTextStorage needs NSMutableAttributedString (the CoreText layer is not installed)");
    const auto storage_class = kit.declare_class("NSTextStorage", "NSMutableAttributedString");
    const auto manager_class = kit.declare_class("NSLayoutManager", "NSObject");
    const auto container_class = kit.declare_class("NSTextContainer", "NSObject");
    const auto shared_class = kit.declare_class("NSTextViewSharedData", "NSObject");
    const auto view_class = kit.text_view;
    for (const auto* name : {"NSTextStorageWillProcessEditingNotification", "NSTextStorageDidProcessEditingNotification"})
        kit.string_constant(std::string("_") + name, name);

    // ---- NSTextStorage ----
    const auto st = storage_class;
    kit.method(st, "initWithCoder:", [k](CpuState& s, GuestMemory&) {
        const auto self = k->send(s.x[0], "init").x0;
        s.x[0] = self;
        const Decoder in{*k, s.x[2]};
        decode_attributed(*k, in, self);
        storages()[self].edited = false;   // decoding is not an edit
        if (const auto delegate = in.object("NSDelegate")) storages()[self].delegate = delegate;
    });
    kit.method(st, "dealloc", [k, st](CpuState& s, GuestMemory&) {
        if (const auto found = storages().find(s.x[0]); found != storages().end()) {
            const auto managers = std::move(found->second.layout_managers);
            storages().erase(found);
            for (const auto manager : managers) {
                if (const auto state = k->objc.find_host_state<LayoutManagerState>(manager); state && state->storage == s.x[0]) state->storage = 0;
                k->release(manager);
            }
        }
        static_cast<void>(k->objc.send_super(s.x[0], st, k->sel("dealloc")));
    }, "v@:");
    kit.method(st, "layoutManagers", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(storages()[s.x[0]].layout_managers); });
    kit.method(st, "addLayoutManager:", [k](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        auto& list = storages()[s.x[0]].layout_managers;
        if (std::find(list.begin(), list.end(), s.x[2]) != list.end()) return;
        list.push_back(k->retain(s.x[2]));
        static_cast<void>(k->send_id(s.x[2], "setTextStorage:", {s.x[0]}));
    }, "v@:@");
    kit.method(st, "removeLayoutManager:", [k](CpuState& s, GuestMemory&) {
        auto& list = storages()[s.x[0]].layout_managers;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found == list.end()) return;
        list.erase(found);
        if (const auto state = k->objc.find_host_state<LayoutManagerState>(s.x[2]); state && state->storage == s.x[0]) state->storage = 0;
        k->release(s.x[2]);
    }, "v@:@");
    kit.method(st, "delegate", [](CpuState& s, GuestMemory&) { s.x[0] = storages()[s.x[0]].delegate; });
    kit.method(st, "setDelegate:", [](CpuState& s, GuestMemory&) { storages()[s.x[0]].delegate = s.x[2]; }, "v@:@");
    kit.method(st, "editedMask", [](CpuState& s, GuestMemory&) { s.x[0] = storages()[s.x[0]].mask; }, "Q@:");
    kit.method(st, "editedRange", [](CpuState& s, GuestMemory&) {
        const auto& info = storages()[s.x[0]];
        s.x[0] = info.edited ? info.location : static_cast<Address>(not_found);
        s.x[1] = info.edited ? info.length : 0;
    }, "{_NSRange=QQ}@:");
    kit.method(st, "changeInLength", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(storages()[s.x[0]].delta); }, "q@:");
    kit.method(st, "fixesAttributesLazily", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    kit.method(st, "edited:range:changeInLength:", [k](CpuState& s, GuestMemory&) {
        note_edit(*k, s.x[0], s.x[2], s.x[3], s.x[4], static_cast<std::int64_t>(s.x[5]));
    }, "v@:Q{_NSRange=QQ}q");
    kit.method(st, "processEditing", [k](CpuState& s, GuestMemory&) { process_editing(*k, s.x[0]); }, "v@:");
    kit.method(st, "beginEditing", [k, st](CpuState& s, GuestMemory&) {
        ++storages()[s.x[0]].depth;
        static_cast<void>(k->objc.send_super(s.x[0], st, k->sel("beginEditing")));
    }, "v@:");
    kit.method(st, "endEditing", [k, st](CpuState& s, GuestMemory&) {
        static_cast<void>(k->objc.send_super(s.x[0], st, k->sel("endEditing")));
        auto& info = storages()[s.x[0]];
        if (info.depth > 0 && --info.depth == 0) process_editing(*k, s.x[0]);
    }, "v@:");
    // The mutations report their edits (super does the change).
    const auto mutation = [&kit, st, k](const char* name, const char* types, auto edit) {
        const std::string selector = name;
        kit.method(st, selector, [k, st, selector, edit](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto before = length_of(*k, self);
            CallRegs registers;
            for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
            static_cast<void>(k->objc.send_super(self, st, k->sel(selector), registers));
            const auto after = length_of(*k, self);
            edit(*k, self, s, before, after);
        }, types);
    };
    const auto chars = [](AppKit& kit2, Address self, std::uint64_t mask, std::size_t location, std::size_t old_length, std::size_t before, std::size_t after) {
        note_edit(kit2, self, mask, location, old_length, static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before));
    };
    mutation("replaceCharactersInRange:withString:", "v@:{_NSRange=QQ}@", [chars](AppKit& kit2, Address self, CpuState& s, std::size_t before, std::size_t after) {
        chars(kit2, self, edited_characters, s.x[2], s.x[3], before, after);
    });
    mutation("deleteCharactersInRange:", "v@:{_NSRange=QQ}", [chars](AppKit& kit2, Address self, CpuState& s, std::size_t before, std::size_t after) {
        chars(kit2, self, edited_characters, s.x[2], s.x[3], before, after);
    });
    mutation("replaceCharactersInRange:withAttributedString:", "v@:{_NSRange=QQ}@", [chars](AppKit& kit2, Address self, CpuState& s, std::size_t before, std::size_t after) {
        chars(kit2, self, edited_characters | edited_attributes, s.x[2], s.x[3], before, after);
    });
    mutation("insertAttributedString:atIndex:", "v@:@Q", [chars](AppKit& kit2, Address self, CpuState& s, std::size_t before, std::size_t after) {
        chars(kit2, self, edited_characters | edited_attributes, s.x[3], 0, before, after);
    });
    mutation("appendAttributedString:", "v@:@", [chars](AppKit& kit2, Address self, CpuState&, std::size_t before, std::size_t after) {
        chars(kit2, self, edited_characters | edited_attributes, before, 0, before, after);
    });
    mutation("setAttributedString:", "v@:@", [chars](AppKit& kit2, Address self, CpuState&, std::size_t before, std::size_t after) {
        chars(kit2, self, edited_characters | edited_attributes, 0, before, before, after);
    });
    mutation("setAttributes:range:", "v@:@{_NSRange=QQ}", [](AppKit& kit2, Address self, CpuState& s, std::size_t, std::size_t) {
        note_edit(kit2, self, edited_attributes, s.x[3], s.x[4], 0);
    });
    mutation("addAttributes:range:", "v@:@{_NSRange=QQ}", [](AppKit& kit2, Address self, CpuState& s, std::size_t, std::size_t) {
        note_edit(kit2, self, edited_attributes, s.x[3], s.x[4], 0);
    });
    mutation("addAttribute:value:range:", "v@:@@{_NSRange=QQ}", [](AppKit& kit2, Address self, CpuState& s, std::size_t, std::size_t) {
        note_edit(kit2, self, edited_attributes, s.x[4], s.x[5], 0);
    });
    mutation("removeAttribute:range:", "v@:@{_NSRange=QQ}", [](AppKit& kit2, Address self, CpuState& s, std::size_t, std::size_t) {
        note_edit(kit2, self, edited_attributes, s.x[3], s.x[4], 0);
    });

    // Scripting views of the text: arrays of immutable attributed substrings
    // (snapshots; AppKit's are live sub-storages, so an edit through one of
    // them fails here instead of reaching the storage).
    const auto substrings = [k](Address self, const std::vector<std::pair<std::size_t, std::size_t>>& ranges) {
        std::vector<Address> values;
        for (const auto& [location, length] : ranges) {
            CallRegs registers;
            registers.x[2] = location;
            registers.x[3] = length;
            values.push_back(k->send(self, "attributedSubstringFromRange:", registers).x0);
        }
        return k->auto_array(values);
    };
    kit.method(st, "attributeRuns", [k, substrings](CpuState& s, GuestMemory& m) {
        const auto total = length_of(*k, s.x[0]);
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        const auto effective = k->process.allocate_guest(16, 8);
        for (std::size_t at = 0; at < total;) {
            CallRegs registers;
            registers.x[2] = at;
            registers.x[3] = effective;
            static_cast<void>(k->send(s.x[0], "attributesAtIndex:effectiveRange:", registers));
            const auto end = std::clamp<std::size_t>(m.read64(effective) + m.read64(effective + 8), at + 1, total);
            ranges.emplace_back(at, end - at);
            at = end;
        }
        k->process.release_guest(effective);
        s.x[0] = substrings(s.x[0], ranges);
    });
    kit.method(st, "paragraphs", [k, substrings](CpuState& s, GuestMemory&) {
        const auto text = k->string16(k->send_id(s.x[0], "string"));
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        std::size_t start = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = text[i];
            if (c != u'\n' && c != u'\r' && c != 0x2029 && c != 0x2028 && c != 0x85) continue;
            if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') ++i;
            ranges.emplace_back(start, i + 1 - start);
            start = i + 1;
        }
        if (start < text.size()) ranges.emplace_back(start, text.size() - start);
        s.x[0] = substrings(s.x[0], ranges);
    });
    kit.method(st, "words", [k, substrings](CpuState& s, GuestMemory&) {
        const auto text = k->string16(k->send_id(s.x[0], "string"));
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        for (std::size_t i = 0; i < text.size();) {
            if (!word_unit(text[i])) { ++i; continue; }
            const auto start = i;
            while (i < text.size() && word_unit(text[i])) ++i;
            ranges.emplace_back(start, i - start);
        }
        s.x[0] = substrings(s.x[0], ranges);
    });
    kit.method(st, "characters", [k, substrings](CpuState& s, GuestMemory&) {
        const auto string = k->send_id(s.x[0], "string");
        const auto total = length_of(*k, s.x[0]);
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        for (std::size_t at = 0; at < total;) {
            CallRegs registers;
            registers.x[2] = at;
            const auto range = k->send(string, "rangeOfComposedCharacterSequenceAtIndex:", registers);
            const auto end = std::clamp<std::size_t>(range.x0 + range.x1, at + 1, total);
            ranges.emplace_back(at, end - at);
            at = end;
        }
        s.x[0] = substrings(s.x[0], ranges);
    });
    // The attribute of the first character; setting it covers the whole text.
    for (const auto& [getter, setter, key] : {std::tuple{"font", "setFont:", "NSFont"}, std::tuple{"foregroundColor", "setForegroundColor:", "NSColor"}}) {
        const std::string name = key;
        kit.method(st, getter, [k, name](CpuState& s, GuestMemory&) {
            if (length_of(*k, s.x[0]) == 0) { s.x[0] = 0; return; }
            CallRegs registers;
            registers.x[2] = k->auto_string(name);
            registers.x[3] = 0;
            registers.x[4] = 0;
            s.x[0] = k->send(s.x[0], "attribute:atIndex:effectiveRange:", registers).x0;
        });
        kit.method(st, setter, [k, name](CpuState& s, GuestMemory&) {
            const auto total = length_of(*k, s.x[0]);
            if (total == 0) return;
            if (s.x[2]) static_cast<void>(k->send_id(s.x[0], "addAttribute:value:range:", {k->auto_string(name), s.x[2], 0, total}));
            else static_cast<void>(k->send_id(s.x[0], "removeAttribute:range:", {k->auto_string(name), 0, total}));
        }, "v@:@");
    }
    // Attributes are fixed as they are set (-fixesAttributesLazily is NO).
    kit.method(st, "ensureAttributesAreFixedInRange:", [](CpuState&, GuestMemory&) {}, "v@:{_NSRange=QQ}");
    kit.method(st, "invalidateAttributesInRange:", [k](CpuState& s, GuestMemory&) {
        if (k->responds(s.x[0], "fixAttributesInRange:")) static_cast<void>(k->send_id(s.x[0], "fixAttributesInRange:", {s.x[2], s.x[3]}));
    }, "v@:{_NSRange=QQ}");
    kit.method(st, "textStorageObserver", [](CpuState& s, GuestMemory&) { s.x[0] = storages()[s.x[0]].observer; });
    kit.method(st, "setTextStorageObserver:", [](CpuState& s, GuestMemory&) { storages()[s.x[0]].observer = s.x[2]; }, "v@:@");

    // ---- NSLayoutManager ----
    const auto lm = manager_class;
    kit.method(lm, "init", [k, lm](CpuState& s, GuestMemory&) {
        s.x[0] = k->objc.send_super(s.x[0], lm, k->sel("init")).x0;
        if (s.x[0]) static_cast<void>(layout_of(*k, s.x[0]));
    });
    kit.method(lm, "initWithCoder:", [k](CpuState& s, GuestMemory&) {
        const auto self = k->send(s.x[0], "init").x0;
        s.x[0] = self;
        const Decoder in{*k, s.x[2]};
        const auto state = layout_of(*k, self);
        state->flags = static_cast<std::uint64_t>(in.integer("NSLMFlags", 0));
        if (const auto delegate = in.object("NSDelegate")) state->delegate = delegate;
        // The storage owns its layout managers.
        if (const auto storage = in.object("NSTextStorage")) static_cast<void>(k->send_id(storage, "addLayoutManager:", {self}));
        if (const auto list = in.object("NSTextContainers"))
            for (const auto container : k->items(list)) if (container) static_cast<void>(k->send_id(self, "addTextContainer:", {container}));
    });
    kit.method(lm, "dealloc", [k, lm](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<LayoutManagerState>(s.x[0])) {
            for (const auto container : std::exchange(state->containers, {})) {
                if (const auto info = k->objc.find_host_state<ContainerState>(container); info && info->layout_manager == s.x[0]) info->layout_manager = 0;
                k->release(container);
            }
            for (auto& run : std::exchange(state->temporary, {})) k->release(run.attributes);
        }
        forget_text_layout(*k, s.x[0]);
        static_cast<void>(k->objc.send_super(s.x[0], lm, k->sel("dealloc")));
    }, "v@:");
    kit.method(lm, "textStorage", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0])->storage; });
    kit.method(lm, "setTextStorage:", [k](CpuState& s, GuestMemory&) {
        const auto state = layout_of(*k, s.x[0]);
        state->storage = s.x[2];
        ++state->generation;
        for (auto& run : std::exchange(state->temporary, {})) k->release(run.attributes);
        for (const auto container : state->containers)
            if (const auto view = container_of(*k, container)->text_view) {
                const auto view_state = text_state(*k, view);
                if (s.x[2]) k->assign(view_state->storage, s.x[2]);
                sync_view(*k, view);
            }
    }, "v@:@");
    kit.method(lm, "replaceTextStorage:", [k](CpuState& s, GuestMemory&) {
        const auto old = layout_of(*k, s.x[0])->storage;
        k->retain(s.x[0]);
        if (old) static_cast<void>(k->send_id(old, "removeLayoutManager:", {s.x[0]}));
        if (s.x[2]) static_cast<void>(k->send_id(s.x[2], "addLayoutManager:", {s.x[0]}));
        k->release(s.x[0]);
    }, "v@:@");
    kit.method(lm, "textContainers", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(layout_of(*k, s.x[0])->containers); });
    kit.method(lm, "insertTextContainer:atIndex:", [k](CpuState& s, GuestMemory&) {
        const auto state = layout_of(*k, s.x[0]);
        if (!s.x[2]) return;
        if (s.x[3] > state->containers.size()) unsupported("-[NSLayoutManager insertTextContainer:atIndex:] index beyond the containers (NSRangeException)");
        state->containers.insert(state->containers.begin() + static_cast<std::ptrdiff_t>(s.x[3]), k->retain(s.x[2]));
        container_of(*k, s.x[2])->layout_manager = s.x[0];
        if (const auto view = container_of(*k, s.x[2])->text_view) {
            const auto view_state = text_state(*k, view);
            k->assign(view_state->layout_manager, s.x[0]);
            if (state->storage) { k->assign(view_state->storage, state->storage); sync_view(*k, view); }
        }
    }, "v@:@Q");
    kit.method(lm, "addTextContainer:", [k](CpuState& s, GuestMemory&) {
        static_cast<void>(k->send_id(s.x[0], "insertTextContainer:atIndex:", {s.x[2], layout_of(*k, s.x[0])->containers.size()}));
    }, "v@:@");
    kit.method(lm, "removeTextContainerAtIndex:", [k](CpuState& s, GuestMemory&) {
        const auto state = layout_of(*k, s.x[0]);
        if (s.x[2] >= state->containers.size()) unsupported("-[NSLayoutManager removeTextContainerAtIndex:] index beyond the containers (NSRangeException)");
        const auto container = state->containers[s.x[2]];
        state->containers.erase(state->containers.begin() + static_cast<std::ptrdiff_t>(s.x[2]));
        if (const auto info = k->objc.find_host_state<ContainerState>(container); info && info->layout_manager == s.x[0]) info->layout_manager = 0;
        k->release(container);
    }, "v@:Q");
    // A container's size or text view changed: its view lays out again.
    for (const auto* name : {"textContainerChangedGeometry:", "textContainerChangedTextView:"})
        kit.method(lm, name, [k](CpuState& s, GuestMemory&) {
            if (!s.x[2]) return;
            ++layout_of(*k, s.x[0])->generation;
            if (const auto view = container_of(*k, s.x[2])->text_view) {
                k->invalidate_view(view);
                const auto state = text_state(*k, view);
                if (!state->field_editor && state->vertically_resizable) static_cast<void>(k->send(view, "sizeToFit"));
            }
        }, "v@:@");
    kit.method(lm, "delegate", [k](CpuState& s, GuestMemory&) { s.x[0] = layout_of(*k, s.x[0])->delegate; });
    kit.method(lm, "setDelegate:", [k](CpuState& s, GuestMemory&) { layout_of(*k, s.x[0])->delegate = s.x[2]; }, "v@:@");
    for (const auto* name : {"firstTextView", "textViewForBeginningOfSelection"})
        kit.method(lm, name, [k](CpuState& s, GuestMemory&) {
            const auto state = layout_of(*k, s.x[0]);
            s.x[0] = 0;
            for (const auto container : state->containers)
                if (const auto view = container_of(*k, container)->text_view) { s.x[0] = view; return; }
        });
    const auto flag = [&kit, lm, k](const char* getter, const char* setter, bool LayoutManagerState::*member) {
        kit.method(lm, getter, [k, member](CpuState& s, GuestMemory&) { return_bool(s, (*layout_of(*k, s.x[0])).*member); }, "B@:");
        kit.method(lm, setter, [k, member](CpuState& s, GuestMemory&) { (*layout_of(*k, s.x[0])).*member = bool_arg(s, 2); }, "v@:B");
    };
    // Layout is always complete here (the text is laid out whole), so
    // non-contiguous layout is allowed but never in effect.
    flag("allowsNonContiguousLayout", "setAllowsNonContiguousLayout:", &LayoutManagerState::non_contiguous);
    kit.method(lm, "hasNonContiguousLayout", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    flag("backgroundLayoutEnabled", "setBackgroundLayoutEnabled:", &LayoutManagerState::background_layout);
    flag("showsInvisibleCharacters", "setShowsInvisibleCharacters:", &LayoutManagerState::shows_invisibles);
    flag("showsControlCharacters", "setShowsControlCharacters:", &LayoutManagerState::shows_control_characters);
    for (const auto* name : {"ensureLayoutForTextContainer:", "ensureLayoutForCharacterRange:", "ensureLayoutForGlyphRange:",
             "invalidateDisplayForCharacterRange:", "invalidateDisplayForGlyphRange:"}) {
        const std::string selector = name;
        kit.method(lm, selector, [k](CpuState& s, GuestMemory&) {
            for (const auto container : layout_of(*k, s.x[0])->containers)
                if (const auto view = container_of(*k, container)->text_view) k->invalidate_view(view);
        }, selector.ends_with("Container:") ? "v@:@" : "v@:{_NSRange=QQ}");
    }
    kit.method(lm, "invalidateLayoutForCharacterRange:actualCharacterRange:", [k](CpuState& s, GuestMemory& m) {
        ++layout_of(*k, s.x[0])->generation;
        if (s.x[4]) { m.write64(s.x[4], s.x[2]); m.write64(s.x[4] + 8, s.x[3]); }
        for (const auto container : layout_of(*k, s.x[0])->containers)
            if (const auto view = container_of(*k, container)->text_view) k->invalidate_view(view);
    }, "v@:{_NSRange=QQ}^{_NSRange=QQ}");
    kit.method(lm, "processEditingForTextStorage:edited:range:changeInLength:invalidatedRange:", [k](CpuState& s, GuestMemory&) {
        const auto state = layout_of(*k, s.x[0]);
        ++state->generation;
        if (s.x[3] & edited_characters) follow_edit(*k, *state, s.x[4], s.x[5], static_cast<std::int64_t>(s.x[6]));
        for (const auto container : state->containers)
            if (const auto view = container_of(*k, container)->text_view) sync_view(*k, view);
    }, "v@:@Q{_NSRange=QQ}q{_NSRange=QQ}");
    // Temporary attributes.
    kit.method(lm, "temporaryAttributesAtCharacterIndex:effectiveRange:", [k](CpuState& s, GuestMemory& m) {
        const auto state = layout_of(*k, s.x[0]);
        const auto total = length_of(*k, state->storage);
        if (s.x[2] >= total) unsupported("-[NSLayoutManager temporaryAttributesAtCharacterIndex:effectiveRange:] index beyond the text (NSRangeException)");
        fit_runs(*k, *state, total);
        std::size_t at = 0;
        for (const auto& run : state->temporary) {
            if (s.x[2] < at + run.length) {
                if (s.x[3]) { m.write64(s.x[3], at); m.write64(s.x[3] + 8, run.length); }
                s.x[0] = run.attributes ? run.attributes : k->autorelease(k->cf.dictionary({}));
                return;
            }
            at += run.length;
        }
        s.x[0] = k->autorelease(k->cf.dictionary({}));
    }, "@@:Q^{_NSRange=QQ}");
    kit.method(lm, "temporaryAttribute:atCharacterIndex:effectiveRange:", [k](CpuState& s, GuestMemory& m) {
        const auto attributes = k->send_id(s.x[0], "temporaryAttributesAtCharacterIndex:effectiveRange:", {s.x[3], s.x[4]});
        s.x[0] = attributes ? k->send_id(attributes, "objectForKey:", {s.x[2]}) : 0;
        static_cast<void>(m);
    }, "@@:@Q^{_NSRange=QQ}");
    kit.method(lm, "setTemporaryAttributes:forCharacterRange:", [k](CpuState& s, GuestMemory&) {
        const auto attributes = s.x[2];
        change_temporary(*k, s.x[0], s.x[3], s.x[4], [k, attributes](Address) {
            return attributes && k->cf.dictionary_entries(attributes).size() ? k->cf.copy(attributes) : 0;
        });
    }, "v@:@{_NSRange=QQ}");
    kit.method(lm, "addTemporaryAttributes:forCharacterRange:", [k](CpuState& s, GuestMemory&) {
        const auto added = s.x[2] ? k->cf.dictionary_entries(s.x[2]) : std::vector<std::pair<Address, Address>>{};
        change_temporary(*k, s.x[0], s.x[3], s.x[4], [k, added](Address old) { return dictionary_with(*k, old, added, {}); });
    }, "v@:@{_NSRange=QQ}");
    kit.method(lm, "addTemporaryAttribute:value:forCharacterRange:", [k](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) unsupported("-[NSLayoutManager addTemporaryAttribute:value:forCharacterRange:] with a nil name or value");
        const std::vector<std::pair<Address, Address>> added{{s.x[2], s.x[3]}};
        change_temporary(*k, s.x[0], s.x[4], s.x[5], [k, added](Address old) { return dictionary_with(*k, old, added, {}); });
    }, "v@:@@{_NSRange=QQ}");
    kit.method(lm, "removeTemporaryAttribute:forCharacterRange:", [k](CpuState& s, GuestMemory&) {
        const auto key = s.x[2];
        change_temporary(*k, s.x[0], s.x[3], s.x[4], [k, key](Address old) { return old ? dictionary_with(*k, old, {}, {key}) : 0; });
    }, "v@:@{_NSRange=QQ}");

    // ---- NSTextContainer additions ----
    const auto tc = container_class;
    kit.method(tc, "initWithCoder:", [k](CpuState& s, GuestMemory&) {
        const auto self = k->send(s.x[0], "init").x0;
        s.x[0] = self;
        const Decoder in{*k, s.x[2]};
        const auto state = container_of(*k, self);
        // NSTextLayoutManager (a TextKit 2 layout manager) is not read: the text is laid out by TextKit 1.
        const auto flags = static_cast<std::uint64_t>(in.integer("NSTCFlags", 1));
        state->tracks_width = (flags & 1) != 0;
        state->tracks_height = (flags & 2) != 0;
        if (in.has("NSWidth")) state->size.width = in.real("NSWidth", state->size.width);
        if (in.has("NSHeight")) state->size.height = in.real("NSHeight", state->size.height);
        if (in.has("NSPadding")) state->padding = in.real("NSPadding", state->padding);
        // The layout manager adds this container to itself when it is decoded.
        if (const auto manager = in.object("NSLayoutManager")) {
            const auto list = layout_of(*k, manager)->containers;
            if (std::find(list.begin(), list.end(), self) == list.end()) static_cast<void>(k->send_id(manager, "addTextContainer:", {self}));
        }
    });
    kit.method(tc, "layoutManager", [k](CpuState& s, GuestMemory&) { s.x[0] = container_of(*k, s.x[0])->layout_manager; });
    kit.method(tc, "setLayoutManager:", [k](CpuState& s, GuestMemory&) { container_of(*k, s.x[0])->layout_manager = s.x[2]; }, "v@:@");
    kit.method(tc, "replaceLayoutManager:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto old = container_of(*k, self)->layout_manager;
        if (old == s.x[2]) return;
        k->retain(self);
        if (old) {
            const auto list = layout_of(*k, old)->containers;
            for (std::size_t i = 0; i < list.size(); ++i)
                if (list[i] == self) { static_cast<void>(k->send_id(old, "removeTextContainerAtIndex:", {i})); break; }
        }
        if (s.x[2]) static_cast<void>(k->send_id(s.x[2], "addTextContainer:", {self}));
        k->release(self);
    }, "v@:@");
    kit.method(tc, "setTextView:", [k](CpuState& s, GuestMemory&) {
        const auto state = container_of(*k, s.x[0]);
        if (state->text_view == s.x[2]) return;
        state->text_view = s.x[2];
        if (s.x[2]) {
            const auto view_state = text_state(*k, s.x[2]);
            k->assign(view_state->container, s.x[0]);
            if (const auto manager = state->layout_manager) {
                k->assign(view_state->layout_manager, manager);
                if (const auto storage = layout_of(*k, manager)->storage) { k->assign(view_state->storage, storage); sync_view(*k, s.x[2]); }
            }
        }
    }, "v@:@");
    kit.method(tc, "isSimpleRectangularTextContainer", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");

    // ---- NSTextViewSharedData (archived) ----
    struct SharedData final : ObjectPayload {
        std::uint64_t flags{}, more_flags{}, checking{};
        Address background{}, insertion{}, selected{}, link{}, marked{}, paragraph{};   // not retained: the nib owns them
        bool has_flags{};
    };
    kit.method(shared_class, "initWithCoder:", [k](CpuState& s, GuestMemory&) {
        const auto self = k->send(s.x[0], "init").x0;
        s.x[0] = self;
        const Decoder in{*k, s.x[2]};
        const auto state = k->objc.host_state<SharedData>(self);
        // Not read: NSAutomaticTextCompletionDisabled (no text completion),
        // NSPreferredTextFinderStyle and NSTextFinder (no find bar),
        // NSTextHighlightAttributes and NSWritingToolsFlags (no Writing Tools).
        state->has_flags = in.has("NSFlags");
        state->flags = static_cast<std::uint64_t>(in.integer("NSFlags", 0));
        state->more_flags = static_cast<std::uint64_t>(in.integer("NSMoreFlags", 0));
        state->checking = static_cast<std::uint64_t>(in.integer("NSTextCheckingTypes", 0));
        state->background = k->retain(in.object("NSBackgroundColor"));
        state->insertion = k->retain(in.object("NSInsertionColor"));
        state->selected = k->retain(in.object("NSSelectedAttributes"));
        // Link attributes hold an NSCursor (the pointing hand); they are read
        // once NSCursor decodes from nibs (gap group G4), else the default
        // link attributes apply.
        const auto cursor_class = k->objc.find_class("NSCursor");
        if (cursor_class && k->guest_call("/usr/lib/libobjc.A.dylib", "_class_getInstanceMethod", {cursor_class, k->sel("initWithCoder:")}))
            state->link = k->retain(in.object("NSLinkAttributes"));
        state->marked = k->retain(in.object("NSMarkedAttributes"));
        state->paragraph = k->retain(in.object("NSDefaultParagraphStyle"));
    });
    kit.method(shared_class, "dealloc", [k, shared_class](CpuState& s, GuestMemory&) {
        if (const auto state = k->objc.find_host_state<SharedData>(s.x[0]))
            for (auto* slot : {&state->background, &state->insertion, &state->selected, &state->link, &state->marked, &state->paragraph})
                k->release(std::exchange(*slot, 0));
        static_cast<void>(k->objc.send_super(s.x[0], shared_class, k->sel("dealloc")));
    }, "v@:");

    // ---- NSTextView ----
    const auto tv = view_class;
    // The view owns a network it makes itself (storage -> layout manager -> container -> view).
    const auto make_network = [k, storage_class, manager_class](Address view) {
        const auto state = text_state(*k, view);
        if (state->layout_manager && state->storage) return;
        const auto container = k->send(view, "textContainer").x0;   // made on demand by text.cpp
        if (!state->storage) {
            const auto storage = k->alloc_init(storage_class);
            state->syncing = true;
            try {
                const auto text = k->auto_string(drawing::utf8(state->value));
                static_cast<void>(k->send_id(storage, "replaceCharactersInRange:withString:", {0, 0, text}));
                Address attributes = state->typing_attributes;
                if (!attributes) {
                    std::vector<std::pair<Address, Address>> entries;
                    entries.emplace_back(k->auto_string("NSFont"), state->font ? state->font : k->font_object(k->font_spec(0)));
                    if (state->text_color) entries.emplace_back(k->auto_string("NSColor"), state->text_color);
                    attributes = k->autorelease(k->cf.dictionary(entries));
                }
                if (!state->value.empty()) static_cast<void>(k->send_id(storage, "setAttributes:range:", {attributes, 0, state->value.size()}));
            } catch (...) { state->syncing = false; throw; }
            state->syncing = false;
            state->storage = storage;   // the view's reference
        }
        if (!state->layout_manager) {
            const auto manager = k->alloc_init(manager_class);
            static_cast<void>(k->send_id(state->storage, "addLayoutManager:", {manager}));
            static_cast<void>(k->send_id(manager, "addTextContainer:", {container}));
            state->layout_manager = manager;   // the view's reference (the storage holds one too)
            container_of(*k, container)->layout_manager = manager;
        }
        container_of(*k, container)->text_view = view;
    };
    kit.method(tv, "textStorage", [k, make_network](CpuState& s, GuestMemory&) {
        make_network(s.x[0]);
        s.x[0] = text_state(*k, s.x[0])->storage;
    });
    kit.method(tv, "layoutManager", [k, make_network](CpuState& s, GuestMemory&) {
        make_network(s.x[0]);
        s.x[0] = text_state(*k, s.x[0])->layout_manager;
    });
    kit.method(tv, "attributedString", [k, make_network](CpuState& s, GuestMemory&) {
        make_network(s.x[0]);
        s.x[0] = text_state(*k, s.x[0])->storage;
    });
    kit.method(tv, "initWithFrame:textContainer:", [k, tv](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        const auto container = s.x[2];
        const auto self = k->send(s.x[0], "initWithFrame:", registers).x0;
        s.x[0] = self;
        if (!self || !container) return;
        static_cast<void>(k->send_id(self, "setTextContainer:", {container}));
        static_cast<void>(tv);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(tv, "setTextContainer:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = text_state(*k, self);
        if (state->container == s.x[2]) return;
        if (const auto old = state->container) {
            if (const auto info = k->objc.find_host_state<ContainerState>(old); info && info->text_view == self) info->text_view = 0;
        }
        k->assign(state->container, s.x[2]);
        k->release(std::exchange(state->layout_manager, 0));
        k->release(std::exchange(state->storage, 0));
        if (s.x[2]) static_cast<void>(k->send_id(s.x[2], "setTextView:", {self}));
        k->invalidate_view(self);
    }, "v@:@");
    kit.method(tv, "replaceTextContainer:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = text_state(*k, self);
        const auto manager = state->layout_manager;
        const auto old = state->container;
        if (!s.x[2] || old == s.x[2]) return;
        k->retain(self);
        if (manager) {
            const auto list = layout_of(*k, manager)->containers;
            for (std::size_t i = 0; i < list.size(); ++i)
                if (list[i] == old) {
                    static_cast<void>(k->send_id(manager, "removeTextContainerAtIndex:", {i}));
                    static_cast<void>(k->send_id(manager, "insertTextContainer:atIndex:", {s.x[2], i}));
                    break;
                }
        }
        if (old) if (const auto info = k->objc.find_host_state<ContainerState>(old); info && info->text_view == self) info->text_view = 0;
        k->assign(state->container, s.x[2]);
        container_of(*k, s.x[2])->text_view = self;
        k->release(self);
    }, "v@:@");
    // Nib decoding: the view's container network and shared settings.
    kit.method(tv, "initWithCoder:", [k, tv](CpuState& s, GuestMemory&) {
        const auto self = super_init_with_coder(*k, s.x[0], tv, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        const auto state = text_state(*k, self);
        const auto tv_flags = static_cast<std::uint64_t>(in.integer("NSTVFlags", 2));
        state->horizontally_resizable = (tv_flags & 1) != 0;
        state->vertically_resizable = (tv_flags & 2) != 0;
        if (const auto size = in.string("NSMaxSize")) {
            CallRegs registers;
            registers.x[2] = k->auto_string("NSMaxSize");
            const auto value = result_size(k->send(s.x[2], "decodeSizeForKey:", registers));
            if (value.width > 0 || value.height > 0) state->max_size = value;
        }
        if (const auto size = in.string("NSMinize")) {   // AppKit's archive key is spelled this way
            CallRegs registers;
            registers.x[2] = k->auto_string("NSMinize");
            state->min_size = result_size(k->send(s.x[2], "decodeSizeForKey:", registers));
        }
        if (const auto delegate = in.object("NSDelegate")) state->delegate = delegate;
        if (const auto color = in.object("NSTextViewTextColor")) k->assign(state->text_color, color);
        if (const auto shared = in.object("NSSharedData")) {
            if (const auto data = k->objc.find_host_state<SharedData>(shared)) {
                if (data->has_flags) {
                    state->selectable = (data->flags & 0x1) != 0;
                    state->editable = (data->flags & 0x2) != 0;
                    state->rich_text = (data->flags & 0x4) != 0;
                    state->field_editor = (data->flags & 0x10) != 0;
                    state->draws_background = (data->flags & 0x800) != 0;
                }
                state->checking_types = data->checking;
                if (data->background) k->assign(state->background_color, data->background);
                if (data->insertion) k->assign(state->insertion_color, data->insertion);
                if (data->selected) k->assign(state->selected_attributes, data->selected);
                if (data->link) k->assign(state->link_attributes, data->link);
                if (data->marked) k->assign(state->marked_attributes, data->marked);
                if (data->paragraph) k->assign(state->default_paragraph, data->paragraph);
            }
        }
        // The container (its layout manager and storage come with it).
        if (const auto container = in.object("NSTextContainer")) {
            const auto info = container_of(*k, container);
            k->assign(state->container, container);
            info->text_view = self;
            if (const auto manager = info->layout_manager) {
                k->assign(state->layout_manager, manager);
                if (const auto storage = layout_of(*k, manager)->storage) {
                    k->assign(state->storage, storage);
                    sync_view(*k, self);
                }
            }
        }
    });
    const auto attributes_property = [&kit, tv, k](const char* getter, const char* setter, Address TextState::*member) {
        kit.method(tv, getter, [k, member](CpuState& s, GuestMemory&) {
            const auto value = (*text_state(*k, s.x[0])).*member;
            s.x[0] = value ? value : k->autorelease(k->cf.dictionary({}));
        });
        kit.method(tv, setter, [k, member](CpuState& s, GuestMemory&) {
            auto& slot = (*text_state(*k, s.x[0])).*member;
            k->release(std::exchange(slot, s.x[2] ? k->cf.copy(s.x[2]) : 0));
            k->invalidate_view(s.x[0]);
        }, "v@:@");
    };
    attributes_property("selectedTextAttributes", "setSelectedTextAttributes:", &TextState::selected_attributes);
    attributes_property("markedTextAttributes", "setMarkedTextAttributes:", &TextState::marked_attributes);
    attributes_property("linkTextAttributes", "setLinkTextAttributes:", &TextState::link_attributes);
    // Typing attributes: the attributes of the next typed text; the font and
    // color are the view's (one style).
    kit.method(tv, "typingAttributes", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        if (state->typing_attributes) { s.x[0] = state->typing_attributes; return; }
        std::vector<std::pair<Address, Address>> entries;
        entries.emplace_back(k->auto_string("NSFont"), state->font ? state->font : k->font_object(k->font_spec(0)));
        if (state->text_color) entries.emplace_back(k->auto_string("NSColor"), state->text_color);
        if (state->default_paragraph) entries.emplace_back(k->auto_string("NSParagraphStyle"), state->default_paragraph);
        s.x[0] = k->autorelease(k->cf.dictionary(entries));
    });
    kit.method(tv, "setTypingAttributes:", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        k->release(std::exchange(state->typing_attributes, s.x[2] ? k->cf.copy(s.x[2]) : 0));
        if (s.x[2] && (state->value.empty() || !state->storage)) {
            if (const auto font = dictionary_get(*k, s.x[2], "NSFont")) k->assign(state->font, font);
            if (const auto color = dictionary_get(*k, s.x[2], "NSColor")) k->assign(state->text_color, color);
            k->invalidate_view(s.x[0]);
        }
    }, "v@:@");
    kit.method(tv, "defaultParagraphStyle", [k](CpuState& s, GuestMemory&) { s.x[0] = text_state(*k, s.x[0])->default_paragraph; });
    kit.method(tv, "setDefaultParagraphStyle:", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        k->release(std::exchange(state->default_paragraph, s.x[2] ? k->send(s.x[2], "copy").x0 : 0));
        if (s.x[2] && k->responds(s.x[2], "alignment")) state->alignment = alignment_from_guest(static_cast<std::int64_t>(k->send(s.x[2], "alignment").x0));
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    kit.method(tv, "insertionPointColor", [k](CpuState& s, GuestMemory&) {
        const auto value = text_state(*k, s.x[0])->insertion_color;
        s.x[0] = value ? value : k->send(k->color, "textColor").x0;
    });
    kit.method(tv, "setInsertionPointColor:", [k](CpuState& s, GuestMemory&) {
        k->assign(text_state(*k, s.x[0])->insertion_color, s.x[2]);
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    kit.method(tv, "enabledTextCheckingTypes", [k](CpuState& s, GuestMemory&) { s.x[0] = text_state(*k, s.x[0])->checking_types; }, "Q@:");
    // Checking itself (spelling, data detectors, replacements) is not
    // provided: the types are kept and no text is checked.
    kit.method(tv, "setEnabledTextCheckingTypes:", [k](CpuState& s, GuestMemory&) { text_state(*k, s.x[0])->checking_types = s.x[2]; }, "v@:Q");
    kit.method(tv, "inlinePredictionType", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(text_state(*k, s.x[0])->inline_prediction); }, "q@:");
    // No predictions are offered (NSTextInputTraitTypeNo behaviour whatever the setting).
    kit.method(tv, "setInlinePredictionType:", [k](CpuState& s, GuestMemory&) {
        text_state(*k, s.x[0])->inline_prediction = static_cast<std::int64_t>(s.x[2]);
    }, "v@:q");
    // Selected ranges: the first is the selection; the others are kept.
    kit.method(tv, "selectedRanges", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        const auto value_class = k->objc.find_class("NSValue");
        std::vector<Address> values;
        const auto add = [&](std::size_t location, std::size_t length) {
            values.push_back(k->send_id(value_class, "valueWithRange:", {location, length}));
        };
        add(state->selection, state->selection_length);
        for (const auto& [location, length] : state->extra_selections) add(location, length);
        s.x[0] = k->auto_array(values);
    });
    const auto set_ranges = [k](Address self, Address ranges) {
        const auto list = ranges ? k->items(ranges) : std::vector<Address>{};
        if (list.empty()) unsupported("-[NSTextView setSelectedRanges:] with no ranges (NSInvalidArgumentException)");
        const auto state = text_state(*k, self);
        std::vector<std::pair<std::size_t, std::size_t>> parsed;
        for (const auto value : list) {
            const auto range = k->send(value, "rangeValue");
            const auto location = std::min<std::size_t>(range.x0, state->value.size());
            parsed.emplace_back(location, std::min<std::size_t>(range.x1, state->value.size() - location));
        }
        state->selection = parsed[0].first;
        state->selection_length = parsed[0].second;
        state->anchor = parsed[0].first;
        state->extra_selections.assign(parsed.begin() + 1, parsed.end());
        k->invalidate_view(self);
        if (state->delegate) {
            CallRegs registers;
            registers.x[2] = k->notification("NSTextViewDidChangeSelectionNotification", self);
            static_cast<void>(k->send_optional(state->delegate, "textViewDidChangeSelection:", registers));
        }
    };
    kit.method(tv, "setSelectedRanges:", [set_ranges](CpuState& s, GuestMemory&) { set_ranges(s.x[0], s.x[2]); }, "v@:@");
    kit.method(tv, "setSelectedRanges:affinity:stillSelecting:", [set_ranges](CpuState& s, GuestMemory&) { set_ranges(s.x[0], s.x[2]); }, "v@:@QB");
    kit.method(tv, "selectionRangeForProposedRange:granularity:", [k](CpuState& s, GuestMemory&) {
        const auto [location, length] = granular_range(text_state(*k, s.x[0])->value, s.x[2], s.x[3], s.x[4]);
        s.x[0] = location;
        s.x[1] = length;
    }, "{_NSRange=QQ}@:{_NSRange=QQ}Q");
    kit.method(tv, "selectionGranularity", [k](CpuState& s, GuestMemory&) { s.x[0] = text_state(*k, s.x[0])->selection_granularity; }, "Q@:");
    // -insertLineBreak: puts a line separator (U+2028) in the text; in a
    // field editor it ends the line like Return.
    kit.method(tv, "insertLineBreak:", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        if (state->field_editor) { static_cast<void>(k->send_id(s.x[0], "insertNewline:", {s.x[2]})); return; }
        static_cast<void>(k->send_id(s.x[0], "insertText:", {k->auto_string("\xE2\x80\xA8")}));
    }, "v@:@");
    kit.method(tv, "insertNewlineIgnoringFieldEditor:", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        if (!state->editable) return;
        const bool field = std::exchange(state->field_editor, false);
        try { static_cast<void>(k->send_id(s.x[0], "insertText:", {k->auto_string("\n")})); } catch (...) { state->field_editor = field; throw; }
        state->field_editor = field;
    }, "v@:@");
    // Completion: the word before the caret, the delegate's completions in a menu.
    kit.method(tv, "rangeForUserCompletion", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        auto end = state->selection + state->selection_length;
        auto start = std::min(end, state->value.size());
        end = start;
        while (start > 0 && word_unit(state->value[start - 1])) --start;
        s.x[0] = start == end ? static_cast<Address>(not_found) : start;
        s.x[1] = end - start;
    }, "{_NSRange=QQ}@:");
    kit.method(tv, "completionsForPartialWordRange:indexOfSelectedItem:", [k](CpuState& s, GuestMemory& m) {
        const auto self = s.x[0];
        const auto state = text_state(*k, self);
        if (s.x[4]) m.write64(s.x[4], 0);
        Address words = k->auto_array({});
        if (state->delegate && k->responds(state->delegate, "textView:completions:forPartialWordRange:indexOfSelectedItem:")) {
            CallRegs registers;
            registers.x[2] = self;
            registers.x[3] = words;
            registers.x[4] = s.x[2];
            registers.x[5] = s.x[3];
            registers.x[6] = s.x[4];
            words = k->send(state->delegate, "textView:completions:forPartialWordRange:indexOfSelectedItem:", registers).x0;
        }
        s.x[0] = words;
    }, "@@:{_NSRange=QQ}^q");
    kit.method(tv, "insertCompletion:forPartialWordRange:movement:isFinal:", [k](CpuState& s, GuestMemory&) {
        if (!bool_arg(s, 6)) return;   // only the final choice changes the text
        static_cast<void>(k->send_id(s.x[0], "insertText:replacementRange:", {s.x[2], s.x[3], s.x[4]}));
    }, "v@:@{_NSRange=QQ}qB");
    kit.method(tv, "complete:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto range = k->send(self, "rangeForUserCompletion");
        if (range.x0 == static_cast<Address>(not_found)) { if (const auto server = window_server_or_null()) server->beep(); return; }
        const auto slot = k->process.allocate_guest(8);
        Address words{};
        try { words = k->send_id(self, "completionsForPartialWordRange:indexOfSelectedItem:", {range.x0, range.x1, slot}); }
        catch (...) { k->process.release_guest(slot); throw; }
        const auto initial = static_cast<std::int64_t>(k->memory().read64(slot));
        k->process.release_guest(slot);
        const auto list = words ? k->items(words) : std::vector<Address>{};
        if (list.empty()) { if (const auto server = window_server_or_null()) server->beep(); return; }
        const auto menu = k->autorelease(k->send_id(k->send(k->menu, "alloc").x0, "initWithTitle:", {k->auto_string("")}));
        for (const auto word : list) static_cast<void>(k->send_id(menu, "addItemWithTitle:action:keyEquivalent:", {word, 0, k->auto_string("")}));
        CallRegs caret;
        caret.x[2] = range.x0 + range.x1;
        const auto anchor = result_rect(k->send(self, "_alchemyCaretRect:", caret));
        const auto chosen = k->popup_menu_tracking(menu, self, anchor, list.size() == 1 ? 0 : std::clamp<std::int64_t>(initial, -1, static_cast<std::int64_t>(list.size()) - 1));
        if (chosen < 0) return;
        CallRegs registers;
        registers.x[2] = list[static_cast<std::size_t>(chosen)];
        registers.x[3] = range.x0;
        registers.x[4] = range.x1;
        registers.x[5] = 0x10;   // NSOtherTextMovement
        registers.x[6] = 1;
        static_cast<void>(k->send(self, "insertCompletion:forPartialWordRange:movement:isFinal:", registers));
    }, "v@:@");

    // ---- NSFontManager / NSFontPanel ----
    if (const auto fm = kit.objc.find_class("NSFontManager")) {
        static Address selected_font{};   // +1 (the shared font manager's)
        static bool selected_multiple{};
        kit.method(fm, "selectedFont", [](CpuState& s, GuestMemory&) { s.x[0] = selected_font; });
        kit.method(fm, "isMultiple", [](CpuState& s, GuestMemory&) { s.x[0] = selected_multiple; }, "B@:");
        kit.method(fm, "setSelectedFont:isMultiple:", [k](CpuState& s, GuestMemory&) {
            k->assign(selected_font, s.x[2]);
            selected_multiple = bool_arg(s, 3);
        }, "v@:@B");
        // The action font changes send: -changeFont: to the target (nil: the
        // first responder), while the manager is enabled. The delegate is weak.
        static Address target{}, delegate{};
        static Address action = k->sel("changeFont:");
        static bool enabled = true;
        kit.method(fm, "target", [](CpuState& s, GuestMemory&) { s.x[0] = target; });
        kit.method(fm, "setTarget:", [](CpuState& s, GuestMemory&) { target = s.x[2]; }, "v@:@");
        kit.method(fm, "action", [](CpuState& s, GuestMemory&) { s.x[0] = action; }, ":@:");
        kit.method(fm, "setAction:", [](CpuState& s, GuestMemory&) { action = s.x[2]; }, "v@::");
        kit.method(fm, "delegate", [](CpuState& s, GuestMemory&) { s.x[0] = delegate; });
        kit.method(fm, "setDelegate:", [](CpuState& s, GuestMemory&) { delegate = s.x[2]; }, "v@:@");
        kit.method(fm, "isEnabled", [](CpuState& s, GuestMemory&) { s.x[0] = enabled; }, "B@:");
        kit.method(fm, "setEnabled:", [](CpuState& s, GuestMemory&) { enabled = bool_arg(s, 2); }, "v@:B");
        kit.method(fm, "sendAction", [k](CpuState& s, GuestMemory&) {
            return_bool(s, enabled && action && k->send_action(action, target, s.x[0]));
        }, "B@:");
        // The font panel: an existing one is answered (none is ever made:
        // the panel UI is not provided), creating one fails explicitly.
        kit.method(fm, "fontPanel:", [k](CpuState& s, GuestMemory&) {
            const auto panel_class = k->objc.find_class("NSFontPanel");
            if (!bool_arg(s, 2)) {
                s.x[0] = panel_class && k->send_bool(panel_class, "sharedFontPanelExists") ? k->send(panel_class, "sharedFontPanel").x0 : 0;
                return;
            }
            if (!panel_class) unsupported("-[NSFontManager fontPanel:YES] (no NSFontPanel)");
            s.x[0] = k->send(panel_class, "sharedFontPanel").x0;
        }, "@@:B");
        // Font changes from the Font menu (NSFontAction): the manager remembers
        // the change and sends its action; the responder's -changeFont: asks
        // -convertFont: to apply that change to each of its fonts.
        static std::uint64_t current_action{}, current_trait{};   // NSNoFontChangeAction
        const auto record = [k](Address manager, std::uint64_t action, std::uint64_t trait) {
            current_action = action;
            current_trait = trait;
            static_cast<void>(k->send(manager, "sendAction"));
        };
        const auto tag_of = [k](Address sender) { return sender ? static_cast<std::uint64_t>(k->send_id(sender, "tag")) : 0; };
        kit.method(fm, "currentFontAction", [](CpuState& s, GuestMemory&) { s.x[0] = current_action; }, "Q@:");
        kit.method(fm, "addFontTrait:", [record, tag_of](CpuState& s, GuestMemory&) { record(s.x[0], 2, tag_of(s.x[2])); }, "v@:@");      // NSAddTraitFontAction
        kit.method(fm, "removeFontTrait:", [record, tag_of](CpuState& s, GuestMemory&) { record(s.x[0], 7, tag_of(s.x[2])); }, "v@:@");   // NSRemoveTraitFontAction
        // The sender's tag is the action: size up 3, size down 4, heavier 5, lighter 6.
        kit.method(fm, "modifyFont:", [record, tag_of](CpuState& s, GuestMemory&) {
            const auto tag = tag_of(s.x[2]);
            record(s.x[0], tag >= 3 && tag <= 6 ? tag : 0, 0);
        }, "v@:@");
        kit.method(fm, "modifyFontViaPanel:", [record](CpuState& s, GuestMemory&) { record(s.x[0], 1, 0); }, "v@:@");   // NSViaPanelFontAction
        // A face (or PostScript name) of the font's family, and the next heavier or lighter member with the same slant.
        const auto family_rows = [k](Address manager, Address font) {
            std::vector<std::vector<Address>> rows;
            const auto list = k->send_id(manager, "availableMembersOfFontFamily:", {k->send_id(font, "familyName")});
            for (const auto row : k->items(list)) rows.push_back(k->items(row));
            return rows;
        };
        const auto sized = [k](Address name, Address font) {
            CallRegs r;
            r.x[2] = name;
            r.set_d(0, k->send(font, "pointSize").d(0));
            return k->send(k->objc.find_class("NSFont"), "fontWithName:size:", r).x0;
        };
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        kit.method(fm, "convertFont:toFace:", [k, family_rows, sized, lower](CpuState& s, GuestMemory&) {
            const auto font = s.x[2];
            const auto face = s.x[3] ? lower(k->text(s.x[3])) : std::string{};
            for (const auto& row : family_rows(s.x[0], font))
                if (row.size() >= 2 && (lower(k->text(row[1])) == face || lower(k->text(row[0])) == face))
                    if (const auto made = sized(row[0], font)) { s.x[0] = made; return; }
            s.x[0] = font;
        }, "@@:@@");
        kit.method(fm, "convertWeight:ofFont:", [k, family_rows, sized](CpuState& s, GuestMemory&) {
            const bool up = bool_arg(s, 2);
            const auto font = s.x[3];
            const auto manager = s.x[0];
            const auto weight = static_cast<std::int64_t>(k->send_id(manager, "weightOfFont:", {font}));
            const auto italic = k->send_id(manager, "traitsOfFont:", {font}) & 1;
            Address best{};
            std::int64_t best_weight = up ? 16 : -1;
            for (const auto& row : family_rows(manager, font)) {
                if (row.size() < 4) continue;
                const auto w = static_cast<std::int64_t>(k->cf.number_value(row[2]).value_or(0));
                const auto t = static_cast<std::uint64_t>(k->cf.number_value(row[3]).value_or(0));
                if ((t & 1) != italic) continue;
                if (up ? (w > weight && w < best_weight) : (w < weight && w > best_weight)) { best = row[0]; best_weight = w; }
            }
            const auto made = best ? sized(best, font) : 0;
            s.x[0] = made ? made : font;
        }, "@@:B@");
        kit.method(fm, "convertFont:", [k](CpuState& s, GuestMemory&) {
            const auto manager = s.x[0], font = s.x[2];
            if (!font) return;
            const auto size = k->send(font, "pointSize").d(0);
            const auto resize = [&](double to) {
                CallRegs r;
                r.x[2] = font;
                r.set_d(0, std::max(1.0, to));
                return k->send(manager, "convertFont:toSize:", r).x0;
            };
            switch (current_action) {
            case 2: s.x[0] = k->send_id(manager, "convertFont:toHaveTrait:", {font, current_trait}); return;
            case 7: s.x[0] = k->send_id(manager, "convertFont:toNotHaveTrait:", {font, current_trait}); return;
            case 3: s.x[0] = resize(size + 1); return;
            case 4: s.x[0] = resize(size - 1); return;
            case 5: s.x[0] = k->send_id(manager, "convertWeight:ofFont:", {1, font}); return;
            case 6: s.x[0] = k->send_id(manager, "convertWeight:ofFont:", {0, font}); return;
            case 1:
                if (const auto panel = k->send_id(manager, "fontPanel:", {0})) { s.x[0] = k->send_id(panel, "panelConvertFont:", {font}); return; }
                s.x[0] = font;
                return;
            default: s.x[0] = font;
            }
        }, "@@:@");
        // The attributes with their font converted.
        kit.method(fm, "convertAttributes:", [k](CpuState& s, GuestMemory&) {
            const auto attributes = s.x[2];
            if (!attributes) { s.x[0] = 0; return; }
            const auto font = dictionary_get(*k, attributes, "NSFont");
            if (!font) { s.x[0] = attributes; return; }
            auto entries = k->cf.dictionary_entries(attributes);
            for (auto& [key, value] : entries)
                if (k->text(key) == "NSFont") value = k->send_id(s.x[0], "convertFont:", {font});
            s.x[0] = k->autorelease(k->cf.dictionary(entries));
        });
        // NSFontTraitMask tests: bold 2 / unbold 4, italic 1 / unitalic 0x1000000, fixed pitch 0x400.
        const auto has_traits = [](std::uint64_t traits, std::uint64_t mask) {
            if ((mask & 2) && !(traits & 2)) return false;
            if ((mask & 4) && (traits & 2)) return false;
            if ((mask & 1) && !(traits & 1)) return false;
            if ((mask & 0x1000000) && (traits & 1)) return false;
            if ((mask & 0x400) && !(traits & 0x400)) return false;
            return true;
        };
        kit.method(fm, "availableFontNamesWithTraits:", [k, has_traits](CpuState& s, GuestMemory&) {
            std::vector<Address> names;
            for (const auto family : k->items(k->send_id(s.x[0], "availableFontFamilies")))
                for (const auto row : k->items(k->send_id(s.x[0], "availableMembersOfFontFamily:", {family}))) {
                    const auto values = k->items(row);
                    if (values.size() >= 4 && has_traits(static_cast<std::uint64_t>(k->cf.number_value(values[3]).value_or(0)), s.x[2])) names.push_back(values[0]);
                }
            s.x[0] = k->auto_array(names);
        }, "@@:Q");
        kit.method(fm, "fontNamed:hasTraits:", [k, has_traits](CpuState& s, GuestMemory&) {
            CallRegs r;
            r.x[2] = s.x[2];
            r.set_d(0, 12);
            const auto font = s.x[2] ? k->send(k->objc.find_class("NSFont"), "fontWithName:size:", r).x0 : 0;
            return_bool(s, font && has_traits(k->send_id(s.x[0], "traitsOfFont:", {font}), s.x[3]));
        }, "B@:@Q");
        kit.method(fm, "localizedNameForFamily:face:", [k](CpuState& s, GuestMemory&) {
            const auto family = s.x[2] ? k->text(s.x[2]) : std::string{};
            s.x[0] = k->auto_string(s.x[3] ? family + " " + k->text(s.x[3]) : family);
        });
        // No font collections exist.
        kit.method(fm, "collectionNames", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array({}); });
        kit.method(fm, "fontDescriptorsInCollection:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        // The Font menu: Show Fonts, Bold, Italic, Underline, Bigger, Smaller (made on request, or the one set).
        static Address font_menu{};   // +1
        kit.method(fm, "setFontMenu:", [k](CpuState& s, GuestMemory&) { k->assign(font_menu, s.x[2]); }, "v@:@");
        kit.method(fm, "fontMenu:", [k](CpuState& s, GuestMemory&) {
            if (!font_menu && bool_arg(s, 2)) {
                font_menu = k->send_id(k->send(k->objc.find_class("NSMenu"), "alloc").x0, "initWithTitle:", {k->auto_string("Font")});
                const auto add = [&](const char* title, const char* action, const char* key, std::uint64_t tag, bool to_manager) {
                    if (!title) { static_cast<void>(k->send_id(font_menu, "addItem:", {k->send_id(k->objc.find_class("NSMenuItem"), "separatorItem")})); return; }
                    const auto item = k->send_id(font_menu, "addItemWithTitle:action:keyEquivalent:", {k->auto_string(title), k->sel(action), k->auto_string(key)});
                    if (to_manager) static_cast<void>(k->send_id(item, "setTarget:", {s.x[0]}));
                    static_cast<void>(k->send_id(item, "setTag:", {tag}));
                };
                add("Show Fonts", "orderFrontFontPanel:", "t", 0, true);
                add("Bold", "addFontTrait:", "b", 2, true);
                add("Italic", "addFontTrait:", "i", 1, true);
                add("Underline", "underline:", "u", 0, false);
                add(nullptr, nullptr, nullptr, 0, false);
                add("Bigger", "modifyFont:", "+", 3, true);
                add("Smaller", "modifyFont:", "-", 4, true);
            }
            s.x[0] = font_menu;
        }, "@@:B");
        for (const auto* name : {"orderFrontFontPanel:", "orderFrontStylesPanel:"}) {
            const std::string selector = name;
            kit.method(fm, selector, [selector](CpuState&, GuestMemory&) { unsupported("-[NSFontManager " + selector + "] (no font panel UI is provided)"); }, "v@:@");
        }
        static Address panel_factory{};
        kit.class_method(fm, "setFontPanelFactory:", [](CpuState& s, GuestMemory&) { panel_factory = s.x[2]; }, "v@:#");   // kept: no panel is made
    }
    if (const auto panel = kit.objc.find_class("NSFontPanel")) {
        // With no panel UI there is never a pending panel change: the font is unchanged.
        kit.method(panel, "panelConvertFont:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[2]; });
        // Settings of a panel that is never shown (kept for the getters).
        static Address accessory{}, panel_font{};   // +1
        static bool panel_enabled = true, panel_multiple{};
        kit.method(panel, "accessoryView", [](CpuState& s, GuestMemory&) { s.x[0] = accessory; });
        kit.method(panel, "setAccessoryView:", [k](CpuState& s, GuestMemory&) { k->assign(accessory, s.x[2]); }, "v@:@");
        kit.method(panel, "isEnabled", [](CpuState& s, GuestMemory&) { return_bool(s, panel_enabled); }, "B@:");
        kit.method(panel, "setEnabled:", [](CpuState& s, GuestMemory&) { panel_enabled = bool_arg(s, 2); }, "v@:B");
        kit.method(panel, "setPanelFont:isMultiple:", [k](CpuState& s, GuestMemory&) {
            k->assign(panel_font, s.x[2]);
            panel_multiple = bool_arg(s, 3);
        }, "v@:@B");
        kit.method(panel, "reloadDefaultFontFamilies", [](CpuState&, GuestMemory&) {}, "v@:");   // the families are read when shown, and it never is
    }
    install_text_layout(kit);
    install_text_actions(kit);
    install_text_finder(kit);
}

} // namespace alchemy::gui::completion_kit::controls
