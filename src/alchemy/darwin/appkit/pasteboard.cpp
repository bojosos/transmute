#include "alchemy/darwin/kit/support/support_kit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <optional>

// Carbon Pasteboard Manager (HIServices, via ApplicationServices), used by
// Qt's QMacPasteboard for the clipboard and the find pasteboard. Pasteboards
// are in-process item/flavor stores; the clipboard pasteboard
// ("com.apple.pasteboard.clipboard") is backed by the host clipboard through
// the window server: text flavors (public.utf8-plain-text,
// public.utf16-plain-text, public.plain-text) are read from and written to the
// host clipboard; other flavors stay in-process (images and private flavors
// are not exported to the host). Promised flavors (NULL data) are resolved
// through the promise keeper when read or on PasteboardResolvePromises.
// Replaces the explicit failures of the headless additions (darwin/headless_additions.hpp).
namespace alchemy::gui::support_kit {
namespace {

constexpr std::string_view application_services = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view clipboard_name = "com.apple.pasteboard.clipboard";
constexpr std::uint64_t no_err = 0;
constexpr std::int64_t bad_index = -25131, bad_item = -25132, bad_flavor = -25133, duplicate_flavor = -25134,
    not_owner = -25135, no_keeper = -25136;
constexpr std::uint64_t sync_modified = 1, sync_owner = 2;

std::uint64_t status(std::int64_t value) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(value)); }

struct Flavor {
    std::optional<std::vector<std::uint8_t>> data;   // nullopt: promised
    std::uint32_t flags{};
};
struct Item {
    Address id{};
    std::vector<std::pair<std::string, Flavor>> flavors;   // insertion order
    Flavor* find(const std::string& name) {
        for (auto& [key, value] : flavors) if (key == name) return &value;
        return nullptr;
    }
};
struct Board final : ObjectPayload {
    std::mutex mutex;
    std::string name;
    std::vector<Item> items;
    bool owner{};
    std::int64_t seen_change{-1};
    Address keeper{}, keeper_context{};
    Address paste_location{};
};

bool text_flavor(const std::string& name) {
    return name == "public.utf8-plain-text" || name == "public.utf16-plain-text" || name == "public.plain-text";
}

std::vector<std::uint8_t> utf16_of(const std::string& utf8) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < utf8.size();) {
        std::uint32_t code = static_cast<unsigned char>(utf8[index]);
        std::size_t extra = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : code >= 0xc0 ? 1 : 0;
        if (extra) code &= (0x3f >> extra);
        for (std::size_t k = 1; k <= extra && index + k < utf8.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(utf8[index + k]) & 0x3f);
        index += extra + 1;
        const auto put = [&](std::uint32_t unit) { bytes.push_back(static_cast<std::uint8_t>(unit)); bytes.push_back(static_cast<std::uint8_t>(unit >> 8)); };
        if (code >= 0x10000) { code -= 0x10000; put(0xd800 | (code >> 10)); put(0xdc00 | (code & 0x3ff)); }
        else put(code);
    }
    return bytes;
}
std::string utf8_of_utf16(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) {
        std::uint32_t code = bytes[index] | (bytes[index + 1] << 8);
        if (code >= 0xd800 && code < 0xdc00 && index + 3 < bytes.size()) {
            const std::uint32_t low = bytes[index + 2] | (bytes[index + 3] << 8);
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            index += 2;
        }
        if (code < 0x80) out.push_back(static_cast<char>(code));
        else if (code < 0x800) { out.push_back(static_cast<char>(0xc0 | (code >> 6))); out.push_back(static_cast<char>(0x80 | (code & 0x3f))); }
        else if (code < 0x10000) { out.push_back(static_cast<char>(0xe0 | (code >> 12))); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (code & 0x3f))); }
        else { out.push_back(static_cast<char>(0xf0 | (code >> 18))); out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f))); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (code & 0x3f))); }
    }
    return out;
}

bool host_clipboard(const Board& board) { return board.name == clipboard_name && window_server_available(); }

// Reloads the clipboard pasteboard from the host clipboard when it changed
// and this process is not its owner. Returns the sync flags.
std::uint64_t synchronize(Board& board) {
    if (!host_clipboard(board)) return board.owner ? sync_owner : 0;
    auto& server = window_server();
    const auto change = server.clipboard_change_count();
    if (change == board.seen_change) return board.owner ? sync_owner : 0;
    board.seen_change = change;
    board.owner = false;
    board.items.clear();
    if (const auto text = server.clipboard_text()) {
        Item item{1, {}};
        item.flavors.emplace_back("public.utf8-plain-text", Flavor{std::vector<std::uint8_t>(text->begin(), text->end()), 0});
        item.flavors.emplace_back("public.utf16-plain-text", Flavor{utf16_of(*text), 0});
        board.items.push_back(std::move(item));
    }
    return sync_modified;
}

void export_text(Board& board, const std::string& flavor, const std::vector<std::uint8_t>& data) {
    if (!host_clipboard(board) || !text_flavor(flavor)) return;
    const auto text = flavor == "public.utf16-plain-text" ? utf8_of_utf16(data) : std::string(data.begin(), data.end());
    auto& server = window_server();
    server.set_clipboard_text(text);
    board.seen_change = server.clipboard_change_count();
}

} // namespace

void install_pasteboard(Support& support) {
    auto* s = &support;
    const auto lib = application_services;
    const auto board_class = support.declare_class("_AlchemyPasteboard", "NSObject");
    auto unique = std::make_shared<std::atomic<std::uint64_t>>(0);
    const auto board_of = [s](Address object) {
        const auto board = object ? s->objc.find_host_state<Board>(object) : nullptr;
        if (!board) fail("Pasteboard: not a PasteboardRef");
        return board;
    };
    const auto make_data = [s](const std::vector<std::uint8_t>& bytes) {
        const auto buffer = s->process.allocate_guest(std::max<std::size_t>(bytes.size(), 1), 16);
        s->memory().write_bytes(buffer, bytes);
        const auto data = s->call(core_foundation, "_CFDataCreate", {0, buffer, bytes.size()});
        s->process.release_guest(buffer);
        return data;
    };
    const auto read_data = [s](Address data) {
        std::vector<std::uint8_t> bytes;
        if (!data) return bytes;
        const auto length = s->call(core_foundation, "_CFDataGetLength", {data});
        const auto pointer = s->call(core_foundation, "_CFDataGetBytePtr", {data});
        bytes.resize(static_cast<std::size_t>(length));
        if (length && pointer) s->memory().read_bytes(pointer, bytes);
        return bytes;
    };
    // Calls the promise keeper for a promised flavor (it answers with PasteboardPutItemFlavor).
    const auto resolve = [s](Address object, Board& board, Address item, const std::string& flavor) {
        Address keeper{}, context{};
        {
            std::lock_guard lock(board.mutex);
            keeper = board.keeper;
            context = board.keeper_context;
        }
        if (!keeper) return false;
        const auto name = s->cf.string(flavor);
        static_cast<void>(EngineAdapter(s->process.calling_engine()).call(keeper, std::array<Address, 4>{object, item, name, context}));
        s->release(name);
        return true;
    };

    support.function(lib, "_PasteboardCreate", [s, board_class, unique](CpuState& c, GuestMemory& m) {
        if (!c.x[1]) { c.x[0] = status(-50); return; }
        auto board = std::make_shared<Board>();
        board->name = c.x[0] ? s->cf.utf8(c.x[0]) : "com.apple.pasteboard.unique." + std::to_string(++*unique);
        m.write64(c.x[1], s->create(board_class, board));
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardSynchronize", [board_of](CpuState& c, GuestMemory&) {
        const auto board = board_of(c.x[0]);
        std::lock_guard lock(board->mutex);
        c.x[0] = synchronize(*board);
    });
    support.function(lib, "_PasteboardClear", [board_of](CpuState& c, GuestMemory&) {
        const auto board = board_of(c.x[0]);
        std::lock_guard lock(board->mutex);
        board->items.clear();
        board->owner = true;
        if (host_clipboard(*board)) board->seen_change = window_server().clipboard_change_count();
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardCopyName", [s, board_of](CpuState& c, GuestMemory& m) {
        const auto board = board_of(c.x[0]);
        if (c.x[1]) m.write64(c.x[1], s->cf.string(board->name));
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardGetItemCount", [board_of](CpuState& c, GuestMemory& m) {
        const auto board = board_of(c.x[0]);
        std::lock_guard lock(board->mutex);
        if (c.x[1]) m.write64(c.x[1], board->items.size());
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardGetItemIdentifier", [board_of](CpuState& c, GuestMemory& m) {
        const auto board = board_of(c.x[0]);
        std::lock_guard lock(board->mutex);
        const auto index = c.x[1];   // 1-based
        if (index < 1 || index > board->items.size()) { c.x[0] = status(bad_index); return; }
        if (c.x[2]) m.write64(c.x[2], board->items[index - 1].id);
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardCopyItemFlavors", [s, board_of](CpuState& c, GuestMemory& m) {
        const auto board = board_of(c.x[0]);
        std::vector<Address> names;
        {
            std::lock_guard lock(board->mutex);
            const auto found = std::find_if(board->items.begin(), board->items.end(), [&](const Item& item) { return item.id == c.x[1]; });
            if (found == board->items.end()) { c.x[0] = status(bad_item); return; }
            for (const auto& [name, flavor] : found->flavors) names.push_back(s->cf.string(name));
        }
        const auto array = s->cf.array(names);
        for (const auto name : names) s->release(name);
        if (c.x[2]) m.write64(c.x[2], array); else s->release(array);
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardGetItemFlavorFlags", [s, board_of](CpuState& c, GuestMemory& m) {
        const auto board = board_of(c.x[0]);
        const auto flavor = c.x[2] ? s->cf.utf8(c.x[2]) : std::string{};
        std::lock_guard lock(board->mutex);
        for (auto& item : board->items) {
            if (item.id != c.x[1]) continue;
            const auto value = item.find(flavor);
            if (!value) { c.x[0] = status(bad_flavor); return; }
            if (c.x[3]) m.write32(c.x[3], value->flags | (value->data ? 0 : 1U << 9 /* kPasteboardFlavorPromised */));
            c.x[0] = no_err;
            return;
        }
        c.x[0] = status(bad_item);
    });
    support.function(lib, "_PasteboardCopyItemFlavorData", [s, board_of, make_data, resolve](CpuState& c, GuestMemory& m) {
        const auto board = board_of(c.x[0]);
        const auto flavor = c.x[2] ? s->cf.utf8(c.x[2]) : std::string{};
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::optional<std::vector<std::uint8_t>> bytes;
            bool promised = false;
            {
                std::lock_guard lock(board->mutex);
                const auto found = std::find_if(board->items.begin(), board->items.end(), [&](const Item& item) { return item.id == c.x[1]; });
                if (found == board->items.end()) { c.x[0] = status(bad_item); return; }
                const auto value = found->find(flavor);
                if (!value) { c.x[0] = status(bad_flavor); return; }
                if (value->data) bytes = value->data;
                else promised = true;
            }
            if (bytes) {
                if (c.x[3]) m.write64(c.x[3], make_data(*bytes));
                c.x[0] = no_err;
                return;
            }
            if (!promised || attempt == 1 || !resolve(c.x[0], *board, c.x[1], flavor)) break;
        }
        c.x[0] = status(no_keeper);
    });
    support.function(lib, "_PasteboardPutItemFlavor", [s, board_of, read_data](CpuState& c, GuestMemory&) {
        const auto board = board_of(c.x[0]);
        const auto flavor = c.x[2] ? s->cf.utf8(c.x[2]) : std::string{};
        std::optional<std::vector<std::uint8_t>> data;
        if (c.x[3]) data = read_data(c.x[3]);
        std::lock_guard lock(board->mutex);
        if (!board->owner) { c.x[0] = status(not_owner); return; }
        auto found = std::find_if(board->items.begin(), board->items.end(), [&](const Item& item) { return item.id == c.x[1]; });
        if (found == board->items.end()) { board->items.push_back({c.x[1], {}}); found = board->items.end() - 1; }
        if (auto existing = found->find(flavor)) {
            if (existing->data) { c.x[0] = status(duplicate_flavor); return; }
            existing->data = data;   // a promise being kept
        } else {
            found->flavors.emplace_back(flavor, Flavor{data, static_cast<std::uint32_t>(c.x[4])});
        }
        if (data) export_text(*board, flavor, *data);
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardSetPromiseKeeper", [board_of](CpuState& c, GuestMemory&) {
        const auto board = board_of(c.x[0]);
        std::lock_guard lock(board->mutex);
        board->keeper = c.x[1];
        board->keeper_context = c.x[2];
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardResolvePromises", [s, board_of, resolve](CpuState& c, GuestMemory&) {
        const auto object = c.x[0];
        const auto board = board_of(object);
        std::vector<std::pair<Address, std::string>> pending;
        {
            std::lock_guard lock(board->mutex);
            for (const auto& item : board->items)
                for (const auto& [name, flavor] : item.flavors) if (!flavor.data) pending.emplace_back(item.id, name);
        }
        for (const auto& [item, name] : pending)
            if (!resolve(object, *board, item, name)) { c.x[0] = status(no_keeper); return; }
        c.x[0] = no_err;
        static_cast<void>(s);
    });
    support.function(lib, "_PasteboardSetPasteLocation", [s, board_of](CpuState& c, GuestMemory&) {
        const auto board = board_of(c.x[0]);
        std::lock_guard lock(board->mutex);
        s->release(std::exchange(board->paste_location, s->retain(c.x[1])));
        c.x[0] = no_err;
    });
    support.function(lib, "_PasteboardCopyPasteLocation", [s, board_of](CpuState& c, GuestMemory& m) {
        const auto board = board_of(c.x[0]);
        std::lock_guard lock(board->mutex);
        if (c.x[1]) m.write64(c.x[1], s->retain(board->paste_location));
        c.x[0] = no_err;
    });
}

} // namespace alchemy::gui::support_kit
