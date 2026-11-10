// Linux host backend, area "ui": desktop queries, the frame timer, shell
// hand-offs and spell checking (include/alchemy/host/ui.hpp). The window
// server, monitors() and window_client_size() are in window_server.cpp; the
// run loop's waits in run_loop.cpp; sample_thread_stack in sample.cpp.
//
// - Idle time is the X ScreenSaver extension's; the keyboard kind follows
//   the XKB layout (_XKB_RULES_NAMES): jp is JIS, us ANSI, others ISO.
// - Appearance and accessibility come from GTK's settings (GTK_THEME, then
//   $XDG_CONFIG_HOME/gtk-3.0/settings.ini); without them the answers are
//   the defaults (light, animations on, no high contrast). A running screen
//   reader is not detected (no AT-SPI client): false.
// - Opening and revealing hand the target to xdg-open (the desktop's file
//   manager shows the folder; it cannot select the file).
// - Spelling is Hunspell's (libhunspell-1.7.so.0, loaded at run time), with
//   the dictionaries in /usr/share/hunspell (apt install hunspell-en-us ...).
//   Without the library there are no languages.
// - No native dialogs (alerts, file panels): Error::unsupported.
#include "alchemy/host/linux/gui/x11.hpp"
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/ui.hpp"

#include <xcb/screensaver.h>

#include <dlfcn.h>
#include <poll.h>
#include <spawn.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

extern char** environ;

namespace alchemy::host::ui {

namespace {

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// ---- GTK settings ----
// The [Settings] keys of GTK 3's settings.ini (the desktop writes them).
std::map<std::string, std::string> gtk_settings() {
    std::map<std::string, std::string> values;
    auto base = environment("XDG_CONFIG_HOME");
    if (base.empty()) base = environment("HOME").empty() ? std::string() : environment("HOME") + "/.config";
    if (base.empty()) return values;
    std::ifstream file(base + "/gtk-3.0/settings.ini");
    std::string line;
    while (std::getline(file, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos || line.empty() || line[0] == '#' || line[0] == '[') continue;
        const auto trim = [](std::string text) {
            const auto first = text.find_first_not_of(" \t\r");
            const auto last = text.find_last_not_of(" \t\r");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        };
        values[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    return values;
}

bool truthy(const std::string& value) { return value == "1" || value == "true" || value == "TRUE"; }

std::string theme_name() {
    if (auto theme = environment("GTK_THEME"); !theme.empty()) return theme;
    const auto settings = gtk_settings();
    const auto it = settings.find("gtk-theme-name");
    return it == settings.end() ? std::string() : it->second;
}

// ---- launching helpers (detached: nobody waits for the desktop's tools) ----
Status launch(const std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t child = 0;
    const int error = posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ);
    if (error) return error == ENOENT ? Error::not_found : Error::io;
    std::thread([child] {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return {};
}

// ---- Hunspell ----
struct Hunspell;
struct HunspellApi {
    Hunspell* (*create)(const char* affix, const char* dictionary) = nullptr;
    int (*spell)(Hunspell*, const char* word) = nullptr;
    int (*suggest)(Hunspell*, char*** list, const char* word) = nullptr;
    void (*free_list)(Hunspell*, char*** list, int count) = nullptr;
    char* (*encoding)(Hunspell*) = nullptr;
};

const HunspellApi* hunspell() {
    static const HunspellApi* const api = []() -> const HunspellApi* {
        void* library = nullptr;
        for (const char* name : {"libhunspell-1.7.so.0", "libhunspell-1.6.so.0", "libhunspell.so"})
            if ((library = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        if (!library) return nullptr;
        auto* loaded = new HunspellApi;
        loaded->create = reinterpret_cast<decltype(loaded->create)>(dlsym(library, "Hunspell_create"));
        loaded->spell = reinterpret_cast<decltype(loaded->spell)>(dlsym(library, "Hunspell_spell"));
        loaded->suggest = reinterpret_cast<decltype(loaded->suggest)>(dlsym(library, "Hunspell_suggest"));
        loaded->free_list = reinterpret_cast<decltype(loaded->free_list)>(dlsym(library, "Hunspell_free_list"));
        loaded->encoding = reinterpret_cast<decltype(loaded->encoding)>(dlsym(library, "Hunspell_get_dic_encoding"));
        if (!loaded->create || !loaded->spell || !loaded->suggest || !loaded->free_list || !loaded->encoding) return nullptr;
        return loaded;
    }();
    return api;
}

constexpr const char* kDictionaries = "/usr/share/hunspell";

// "en-US" for en_US.dic beside en_US.aff.
std::vector<std::string> dictionary_languages() {
    std::set<std::string> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(kDictionaries, error)) {
        const auto path = entry.path();
        if (path.extension() != ".dic") continue;
        auto affix = path;
        affix.replace_extension(".aff");
        if (!std::filesystem::exists(affix, error)) continue;
        auto tag = path.stem().string();
        std::replace(tag.begin(), tag.end(), '_', '-');
        found.insert(tag);
    }
    return {found.begin(), found.end()};
}

struct Speller {
    Hunspell* handle{};
    bool utf8{};
};

// One speller per language, made on first use (never freed: they live for the process).
const Speller* speller(std::string_view language) {
    static std::mutex mutex;
    static auto* spellers = new std::map<std::string, Speller, std::less<>>;
    const auto* api = hunspell();
    if (!api) return nullptr;
    std::lock_guard lock(mutex);
    if (const auto it = spellers->find(language); it != spellers->end()) return it->second.handle ? &it->second : nullptr;
    std::string stem(language);
    std::replace(stem.begin(), stem.end(), '-', '_');
    Speller made;
    const auto base = std::string(kDictionaries) + "/" + stem;
    std::error_code error;
    if (stem.find('/') == std::string::npos && std::filesystem::exists(base + ".dic", error) && std::filesystem::exists(base + ".aff", error)) {
        made.handle = api->create((base + ".aff").c_str(), (base + ".dic").c_str());
        if (made.handle) {
            const char* encoding = api->encoding(made.handle);
            made.utf8 = encoding && (std::strcmp(encoding, "UTF-8") == 0 || std::strcmp(encoding, "utf-8") == 0);
        }
    }
    auto& slot = (*spellers)[std::string(language)] = made;
    return slot.handle ? &slot : nullptr;
}

// UTF-16 to the dictionary's encoding (UTF-8, else Latin-1 with '?' past it).
std::string encode(const Speller& speller, std::u16string_view word) {
    std::string out;
    for (std::size_t i = 0; i < word.size(); ++i) {
        char32_t c = word[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < word.size() && word[i + 1] >= 0xdc00 && word[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (word[++i] - 0xdc00);
        if (!speller.utf8) {
            out += c < 0x100 ? static_cast<char>(c) : '?';
        } else if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return out;
}

std::u16string decode(const Speller& speller, const char* text) {
    std::u16string out;
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p;) {
        char32_t c = *p++;
        if (speller.utf8 && c >= 0xc0) {
            const int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : 0x1f;
            for (int k = 0; k < extra && (*p & 0xc0) == 0x80; ++k) c = (c << 6) | (*p++ & 0x3f);
        }
        if (c >= 0x10000) {
            out += static_cast<char16_t>(0xd800 + ((c - 0x10000) >> 10));
            out += static_cast<char16_t>(0xdc00 + ((c - 0x10000) & 0x3ff));
        } else {
            out += static_cast<char16_t>(c);
        }
    }
    return out;
}

// Word characters: letters (anything past ASCII counts) and inner apostrophes.
bool word_character(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c >= 0xc0 || c == u'\'' || c == 0x2019;
}

} // namespace

// ---- desktop queries ----
Result<std::uint32_t> idle_ms() {
    linux_gui::AnyConnection any;
    if (!any.connection) return Error::unsupported;
    const auto* extension = xcb_get_extension_data(any.connection, &xcb_screensaver_id);
    if (!extension || !extension->present) return Error::unsupported;
    auto* reply = xcb_screensaver_query_info_reply(any.connection, xcb_screensaver_query_info(any.connection, any.screen->root), nullptr);
    if (!reply) return Error::unsupported;
    const std::uint32_t idle = reply->ms_since_user_input;
    std::free(reply);
    return idle;
}

Result<KeyboardKind> keyboard_kind() {
    linux_gui::AnyConnection any;
    std::string layout;
    if (any.connection) {
        static constexpr char name[] = "_XKB_RULES_NAMES";
        if (auto* atom = xcb_intern_atom_reply(any.connection, xcb_intern_atom(any.connection, 1, sizeof(name) - 1, name), nullptr)) {
            if (atom->atom != XCB_NONE)
                if (auto* reply = xcb_get_property_reply(
                        any.connection, xcb_get_property(any.connection, 0, any.screen->root, atom->atom, XCB_ATOM_STRING, 0, 1024), nullptr)) {
                    // rules \0 model \0 layout \0 variant \0 options
                    const auto* data = static_cast<const char*>(xcb_get_property_value(reply));
                    const std::string all(data, static_cast<std::size_t>(xcb_get_property_value_length(reply)));
                    std::size_t at = 0;
                    for (int field = 0; field < 2 && at != std::string::npos; ++field) at = all.find('\0', at) + 1;
                    if (at != std::string::npos && at < all.size()) layout = all.substr(at, all.find('\0', at) - at);
                    std::free(reply);
                }
            std::free(atom);
        }
    }
    if (layout.empty()) return KeyboardKind::ansi;   // no X keyboard: the common case
    layout = layout.substr(0, layout.find(','));     // the first (active by default) layout
    if (layout == "jp") return KeyboardKind::jis;
    if (layout == "us" || layout == "cn" || layout == "kr" || layout == "th" || layout == "il") return KeyboardKind::ansi;
    return KeyboardKind::iso;
}

Result<bool> reduce_motion() {
    const auto settings = gtk_settings();
    const auto it = settings.find("gtk-enable-animations");
    return it != settings.end() && !truthy(it->second);
}

Result<bool> high_contrast() {
    const auto theme = theme_name();
    return theme.find("HighContrast") != std::string::npos || theme.find("high-contrast") != std::string::npos;
}

Result<bool> screen_reader() { return false; }

Result<bool> dark_appearance() {
    const auto theme = theme_name();
    if (theme.find(":dark") != std::string::npos || theme.find("-dark") != std::string::npos || theme.find("-Dark") != std::string::npos)
        return true;
    const auto settings = gtk_settings();
    const auto it = settings.find("gtk-application-prefer-dark-theme");
    return it != settings.end() && truthy(it->second);
}

Result<std::array<std::uint8_t, 3>> pixel_under_pointer() {
    linux_gui::AnyConnection any;
    if (!any.connection) return Error::unsupported;
    auto* pointer = xcb_query_pointer_reply(any.connection, xcb_query_pointer(any.connection, any.screen->root), nullptr);
    if (!pointer) return Error::unsupported;
    const auto x = pointer->root_x, y = pointer->root_y;
    std::free(pointer);
    auto* image = xcb_get_image_reply(
        any.connection, xcb_get_image(any.connection, XCB_IMAGE_FORMAT_Z_PIXMAP, any.screen->root, x, y, 1, 1, ~0u), nullptr);
    if (!image) return Error::unsupported;
    std::array<std::uint8_t, 3> rgb{};
    if (xcb_get_image_data_length(image) >= 4 && image->depth >= 24) {
        const auto* bgrx = xcb_get_image_data(image);   // TrueColor 24/32: BGRX on little-endian hosts
        rgb = {bgrx[2], bgrx[1], bgrx[0]};
    }
    std::free(image);
    return rgb;
}

// ---- the shell ----
Status shell_open(std::string_view target, std::string_view verb, std::string_view) {
    if (target.empty()) return Error::invalid_argument;
    if (!verb.empty() && verb != "open") return Error::unsupported;   // xdg-open only opens
    return launch({"xdg-open", std::string(target)});
}

Status reveal_file(std::string_view path) {
    if (path.empty()) return Error::invalid_argument;
    const auto folder = std::filesystem::path(std::string(path)).parent_path();
    return launch({"xdg-open", folder.empty() ? std::string(".") : folder.string()});
}

Status reveal_files(std::span<const std::string> paths) {
    std::set<std::string> folders;
    for (const auto& path : paths) {
        const auto folder = std::filesystem::path(path).parent_path().string();
        if (folders.insert(folder).second)
            if (const auto opened = launch({"xdg-open", folder.empty() ? std::string(".") : folder}); !opened) return opened;
    }
    return {};
}

void add_recent_document(std::string_view) {}

// No desktop picture this backend can read without the desktop's own service.
Result<std::string> desktop_picture() { return std::string(); }
Status set_desktop_picture(std::string_view) { return Error::unsupported; }

// ---- the frame timer: a timerfd ----
Result<FrameTimer> FrameTimer::create() {
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) return Error::no_memory;
    FrameTimer timer;
    timer.handle_ = OwnedHandle(linux_backend::handle_of(fd));
    return timer;
}
FrameTimer::FrameTimer() = default;
FrameTimer::FrameTimer(FrameTimer&&) noexcept = default;
FrameTimer& FrameTimer::operator=(FrameTimer&&) noexcept = default;
FrameTimer::~FrameTimer() = default;

Status FrameTimer::wait(std::uint64_t delay_ns, std::uint32_t limit_ms) {
    if (!handle_) return Error::invalid_argument;
    const int fd = linux_backend::fd_of(handle_.get());
    itimerspec spec{};
    const auto delay = std::max<std::uint64_t>(delay_ns, 1);
    spec.it_value.tv_sec = static_cast<time_t>(delay / 1'000'000'000);
    spec.it_value.tv_nsec = static_cast<long>(delay % 1'000'000'000);
    if (timerfd_settime(fd, 0, &spec, nullptr) != 0) return Error::io;
    pollfd entry{fd, POLLIN, 0};
    int ready = 0;
    while ((ready = ::poll(&entry, 1, static_cast<int>(std::min<std::uint32_t>(limit_ms, 0x7fffffff)))) < 0 && errno == EINTR) {
    }
    std::uint64_t expirations = 0;
    static_cast<void>(::read(fd, &expirations, sizeof(expirations)));
    const itimerspec off{};
    timerfd_settime(fd, 0, &off, nullptr);
    return {};
}

// ---- dialogs ----
Result<AlertAnswer> show_alert(const AlertDialog& dialog) {
    if (dialog.buttons.empty()) return Error::invalid_argument;
    return Error::unsupported;   // no native alerts on this backend
}
Result<std::optional<std::vector<std::string>>> run_file_dialog(const FileDialog&) { return Error::unsupported; }
std::vector<std::uintptr_t> disable_windows(std::span<const std::uintptr_t>, std::uintptr_t) { return {}; }
void enable_windows(std::span<const std::uintptr_t>) {}

// ---- spell checking ----
Result<std::vector<std::string>> spelling_languages() {
    if (!hunspell()) return std::vector<std::string>{};
    return dictionary_languages();
}

Result<std::vector<Misspelling>> check_spelling(std::string_view language, std::u16string_view text) {
    std::vector<Misspelling> found;
    const auto* checker = speller(language);
    if (!checker) return found;   // an unknown language finds nothing
    for (std::size_t i = 0; i < text.size();) {
        if (!word_character(text[i]) || text[i] == u'\'' || text[i] == 0x2019) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && word_character(text[end])) ++end;
        std::size_t length = end - i;
        while (length && (text[i + length - 1] == u'\'' || text[i + length - 1] == 0x2019)) --length;   // trailing quote
        const auto word = encode(*checker, text.substr(i, length));
        if (!word.empty() && !hunspell()->spell(checker->handle, word.c_str())) found.push_back({i, length, false, {}});
        i = end;
    }
    return found;
}

Result<std::vector<std::u16string>> spelling_suggestions(std::string_view language, std::u16string_view word) {
    std::vector<std::u16string> guesses;
    const auto* checker = speller(language);
    if (!checker || word.empty()) return guesses;
    char** list = nullptr;
    const auto* api = hunspell();
    const int count = api->suggest(checker->handle, &list, encode(*checker, word).c_str());
    for (int i = 0; i < count; ++i) guesses.push_back(decode(*checker, list[i]));
    if (list) api->free_list(checker->handle, &list, count);
    return guesses;
}

} // namespace alchemy::host::ui
