// Guest DLL search path: private side-by-side assemblies, application
// directory, configured DLL directories, AddDllDirectory directories and the
// current directory, then PATH. KnownDLLs and API-set names resolve to the host.

#include "fx/loader/loader_internal.hpp"

#include <algorithm>
#include <cwctype>

namespace transmute::fx::loader {

// ---- strings ------------------------------------------------------------------

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    bool ascii = true;
    for (char c : text) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
    if (ascii) return std::wstring(text.begin(), text.end());
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    if (n <= 0) {
        code_page = CP_ACP;
        n = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    bool ascii = true;
    for (wchar_t c : text) ascii = ascii && c < 0x80;
    if (ascii) {
        std::string out(text.size(), '\0');
        for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<char>(text[i]);
        return out;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr,
                                      0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n,
                            nullptr, nullptr);
    }
    return out;
}

std::wstring lower(std::wstring_view text) {
    std::wstring out(text);
    bool ascii = true;
    for (auto& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        else if (c >= 0x80) ascii = false;
    }
    if (!ascii && !out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

bool has_path_separator(std::wstring_view name) {
    return name.find_first_of(L"\\/") != std::wstring_view::npos ||
           (name.size() >= 2 && name[1] == L':');
}

std::wstring file_name_of(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) path.remove_prefix(slash + 1);
    else if (path.size() >= 2 && path[1] == L':') path.remove_prefix(2);
    return std::wstring(path);
}

std::wstring directory_of(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return {};
    if (slash == 2 && path[1] == L':') return std::wstring(path.substr(0, 3));
    return std::wstring(path.substr(0, slash));
}

std::wstring full_path(std::wstring_view path) {
    std::wstring input(path);
    DWORD n = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (n == 0) return input;
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(input.c_str(), n, out.data(), nullptr);
    out.resize(n);
    return out;
}

bool is_api_set_name(std::wstring_view name) {
    if (name.size() < 4) return false;
    const auto prefix = lower(name.substr(0, 4));
    return prefix == L"api-" || prefix == L"ext-";
}

std::wstring normalize_dll_name(std::wstring_view name) {
    std::wstring out(name);
    if (!out.empty() && out.back() == L'.') {
        out.pop_back();   // "foo." means "no extension"
        return out;
    }
    const auto file = file_name_of(out);
    if (file.find(L'.') == std::wstring::npos) out += L".dll";
    return out;
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// ---- KnownDLLs ------------------------------------------------------------------

bool is_known_dll(const std::wstring& lower_name) {
    auto& s = state();
    if (!s.known_dlls_loaded) {
        s.known_dlls_loaded = true;
        s.known_dlls.insert(L"ntdll.dll");
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs", 0,
                          KEY_READ, &key) == ERROR_SUCCESS) {
            for (DWORD i = 0;; ++i) {
                wchar_t value_name[256];
                DWORD name_size = 256;
                wchar_t data[MAX_PATH];
                DWORD data_size = sizeof(data);
                DWORD type = 0;
                const LONG r = RegEnumValueW(key, i, value_name, &name_size, nullptr, &type,
                                             reinterpret_cast<BYTE*>(data), &data_size);
                if (r == ERROR_NO_MORE_ITEMS) break;
                if (r != ERROR_SUCCESS || type != REG_SZ) continue;
                if (lower(value_name).rfind(L"dlldirectory", 0) == 0) continue;
                const std::size_t chars = data_size / sizeof(wchar_t);
                std::wstring value(data, chars && data[chars - 1] == 0 ? chars - 1 : chars);
                s.known_dlls.insert(lower(value));
            }
            RegCloseKey(key);
        }
    }
    return s.known_dlls.count(lower_name) != 0;
}

// ---- search directories ------------------------------------------------------------

namespace {

std::vector<std::wstring> search_directories(const SearchContext& search) {
    auto& s = state();
    std::vector<std::wstring> dirs;
    constexpr std::uint32_t kSearchMask = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                          LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                          LOAD_LIBRARY_SEARCH_USER_DIRS |
                                          LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    const std::uint32_t flags = search.flags;
    if (flags & kSearchMask) {
        if ((flags & LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) && !search.dll_load_dir.empty()) {
            dirs.push_back(search.dll_load_dir);
        }
        if (flags & (LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
            if (!s.app_dir.empty()) dirs.push_back(s.app_dir);
            for (const auto& d : s.dll_dirs) dirs.push_back(d);
        }
        if (flags & (LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
            for (const auto& d : s.user_dirs) dirs.push_back(d.second);
        }
        return dirs;   // LOAD_LIBRARY_SEARCH_SYSTEM32 only reaches host DLLs
    }
    if ((flags & LOAD_WITH_ALTERED_SEARCH_PATH) && !search.dll_load_dir.empty()) {
        dirs.push_back(search.dll_load_dir);
    } else if (!s.app_dir.empty()) {
        dirs.push_back(s.app_dir);
    }
    for (const auto& d : s.dll_dirs) dirs.push_back(d);
    if (!s.set_dll_directory.empty()) {
        dirs.push_back(s.set_dll_directory);
    } else {
        wchar_t cwd[MAX_PATH * 2];
        const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd);
        if (n > 0 && n < std::size(cwd)) dirs.emplace_back(cwd, n);
    }
    const DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (size > 1) {
        std::wstring path(size, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"PATH", path.data(), size);
        if (length > 0 && length < size) {
            path.resize(length);
            for (std::size_t start = 0; start < path.size();) {
                auto end = path.find(L';', start);
                if (end == std::wstring::npos) end = path.size();
                if (end > start) dirs.push_back(path.substr(start, end - start));
                start = end + 1;
            }
        }
    }
    return dirs;
}

std::wstring join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

template <typename Accept>
std::wstring find_file(std::wstring_view name, const SearchContext& search, Accept accept) {
    if (name.empty()) return {};
    const std::wstring normalized = normalize_dll_name(name);
    if (has_path_separator(normalized)) {
        const std::wstring path = full_path(normalized);
        return file_exists(path) && accept(path) ? path : std::wstring{};
    }
    auto& s = state();
    if (!s.sxs_loaded && !s.image_path.empty()) load_sxs_for_exe(s.image_path, nullptr);
    const std::wstring key = lower(normalized);
    if (const auto it = s.sxs.find(key); it != s.sxs.end()) {
        if (file_exists(it->second) && accept(it->second)) return it->second;
    }
    for (const auto& dir : search_directories(search)) {
        std::wstring path = join(dir, normalized);
        if (file_exists(path) && accept(path)) return full_path(path);
    }
    return {};
}

} // namespace

std::wstring find_guest_file(std::wstring_view name, const SearchContext& search) {
    return find_file(name, search, [](const std::wstring& path) {
        return probe_pe_file(path) == kMachineArm64;
    });
}

std::wstring find_any_file(std::wstring_view name, const SearchContext& search) {
    return find_file(name, search, [](const std::wstring&) { return true; });
}

// ---- side-by-side private assemblies ---------------------------------------------

namespace {

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return out;
}

// Value of attribute `name` inside the tag text `tag` ("" if absent).
std::string attribute(std::string_view tag, std::string_view name, bool* present = nullptr) {
    const std::string lowered = lower_ascii(tag);
    const std::string needle = lower_ascii(name);
    std::size_t at = 0;
    while ((at = lowered.find(needle, at)) != std::string::npos) {
        const bool boundary = at > 0 && (lowered[at - 1] == ' ' || lowered[at - 1] == '\t' ||
                                         lowered[at - 1] == '\r' || lowered[at - 1] == '\n');
        std::size_t p = at + needle.size();
        while (p < lowered.size() && (lowered[p] == ' ' || lowered[p] == '\t')) ++p;
        if (boundary && p < lowered.size() && lowered[p] == '=') {
            ++p;
            while (p < lowered.size() && (lowered[p] == ' ' || lowered[p] == '\t')) ++p;
            if (p < lowered.size() && (tag[p] == '"' || tag[p] == '\'')) {
                const char quote = tag[p];
                const std::size_t close = tag.find(quote, p + 1);
                if (close != std::string_view::npos) {
                    if (present) *present = true;
                    return std::string(tag.substr(p + 1, close - p - 1));
                }
            }
        }
        at += needle.size();
    }
    if (present) *present = false;
    return {};
}

// Tag texts ("<x ...>") whose element name ends with `element` (namespace
// prefixes allowed).
std::vector<std::string_view> tags(std::string_view xml, std::string_view element) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while ((at = xml.find('<', at)) != std::string_view::npos) {
        const std::size_t close = xml.find('>', at);
        if (close == std::string_view::npos) break;
        std::string_view tag = xml.substr(at, close - at + 1);
        std::size_t name_end = 1;
        while (name_end < tag.size() && tag[name_end] != ' ' && tag[name_end] != '\t' &&
               tag[name_end] != '\r' && tag[name_end] != '\n' && tag[name_end] != '/' &&
               tag[name_end] != '>') {
            ++name_end;
        }
        std::string_view tag_name = tag.substr(1, name_end - 1);
        if (const auto colon = tag_name.find(':'); colon != std::string_view::npos) {
            tag_name.remove_prefix(colon + 1);
        }
        if (lower_ascii(tag_name) == lower_ascii(element)) out.push_back(tag);
        at = close + 1;
    }
    return out;
}

std::string decode_manifest(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        std::wstring_view wide(reinterpret_cast<const wchar_t*>(bytes.data() + 2),
                               (bytes.size() - 2) / 2);
        return narrow(wide);
    }
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef) text.erase(0, 3);
    return text;
}

bool read_text_file(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
    std::vector<std::uint8_t> bytes;
    if (ok) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        ok = bytes.empty() ||
             (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) &&
              got == bytes.size());
    }
    CloseHandle(file);
    if (ok) out = decode_manifest(bytes);
    return ok;
}

void load_assembly(const std::string& name, const std::wstring& app_dir) {
    const std::wstring wide = widen(name);
    const std::wstring candidates[] = {
        app_dir + L"\\" + wide + L".manifest",
        app_dir + L"\\" + wide + L"\\" + wide + L".manifest",
    };
    for (const auto& path : candidates) {
        std::string xml;
        if (!read_text_file(path, xml)) continue;
        const std::wstring dir = directory_of(path);
        for (const auto tag : tags(xml, "file")) {
            const std::string file = attribute(tag, "name");
            if (file.empty()) continue;
            const std::wstring wfile = widen(file);
            state().sxs.emplace(lower(file_name_of(wfile)), dir + L"\\" + wfile);
        }
        return;
    }
}

std::string xml_escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"') out += "&quot;";
        else if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else out.push_back(c);
    }
    return out;
}

// Builds an activation context that only requests the shared (WinSxS)
// assemblies, with any processor architecture, for host DLL loads. The
// guest's private assemblies are ARM64 and must never reach the host loader.
void create_host_activation_context(const std::vector<std::string>& shared) {
    release_host_activation_context();
    if (shared.empty()) return;
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n";
    for (const auto& tag : shared) {
        const std::string name = attribute(tag, "name");
        const std::string version = attribute(tag, "version");
        const std::string token = attribute(tag, "publicKeyToken");
        std::string type = attribute(tag, "type");
        if (type.empty()) type = "win32";
        if (name.empty() || version.empty()) continue;
        xml += "<dependency><dependentAssembly><assemblyIdentity type=\"" + xml_escape(type) +
               "\" name=\"" + xml_escape(name) + "\" version=\"" + xml_escape(version) +
               "\" processorArchitecture=\"*\" publicKeyToken=\"" + xml_escape(token) +
               "\" language=\"*\"/></dependentAssembly></dependency>\r\n";
    }
    xml += "</assembly>\r\n";
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return;
    static std::atomic<unsigned> counter{0};
    const std::wstring path = std::wstring(temp) + L"transmute-fx-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(counter.fetch_add(1)) + L".manifest";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool ok = WriteFile(file, xml.data(), static_cast<DWORD>(xml.size()), &written, nullptr) &&
                    written == xml.size();
    CloseHandle(file);
    if (ok) {
        ACTCTXW context{};
        context.cbSize = sizeof(context);
        context.lpSource = path.c_str();
        state().host_actctx = CreateActCtxW(&context);
        if (state().host_actctx == INVALID_HANDLE_VALUE) {
            LoaderDiagnostic d;
            d.kind = LoaderDiagnostic::Kind::note;
            d.message = "CreateActCtx for shared assemblies failed (" + std::to_string(GetLastError()) + ")";
            add_diagnostic(std::move(d));
        }
    }
    DeleteFileW(path.c_str());
}

} // namespace

void release_host_activation_context() {
    auto& s = state();
    if (s.host_actctx != INVALID_HANDLE_VALUE) ReleaseActCtx(s.host_actctx);
    s.host_actctx = INVALID_HANDLE_VALUE;
}

HostActivation::HostActivation() {
    const HANDLE context = state().host_actctx;
    if (context != INVALID_HANDLE_VALUE) active_ = ActivateActCtx(context, &cookie_) != FALSE;
}

HostActivation::~HostActivation() {
    if (active_) DeactivateActCtx(0, cookie_);
}

void load_sxs_from_manifest(std::string_view manifest, const std::wstring& app_dir) {
    // <dependency><dependentAssembly><assemblyIdentity name=".."/>: private
    // assemblies carry no publicKeyToken and supply guest DLLs; shared ones
    // (publicKeyToken, WinSxS) go into a host activation context.
    std::size_t at = 0;
    const std::string lowered = lower_ascii(manifest);
    std::vector<std::string> shared;
    while ((at = lowered.find("dependentassembly", at)) != std::string::npos) {
        std::size_t end = lowered.find("dependentassembly", at + 17);
        if (end == std::string::npos) end = lowered.size();
        const std::string_view block = manifest.substr(at, end - at);
        at = end + 17;
        for (const auto tag : tags(block, "assemblyIdentity")) {
            bool has_token = false;
            attribute(tag, "publicKeyToken", &has_token);
            const std::string name = attribute(tag, "name");
            if (name.empty()) continue;
            if (has_token) shared.emplace_back(tag);
            else load_assembly(name, app_dir);
        }
    }
    create_host_activation_context(shared);
}

void load_sxs_for_exe(const std::wstring& exe_path, const MappedImage* image) {
    auto& s = state();
    if (s.sxs_loaded) return;
    s.sxs_loaded = true;
    const std::wstring app_dir = directory_of(exe_path);
    std::string manifest;
    if (image && image->base) {
        constexpr std::uint16_t kRtManifest = 24;
        const auto bytes = find_resource(image->base, image->headers.size_of_image,
                                         image->headers.dir(dir_resource), kRtManifest, 1);
        if (!bytes.empty()) manifest = decode_manifest(bytes);
    }
    if (manifest.empty() && !image) {
        // Not mapped (configuration without launch): read the embedded
        // manifest through the host resource loader.
        HMODULE h = LoadLibraryExW(exe_path.c_str(), nullptr,
                                   LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        if (h) {
            if (HRSRC r = FindResourceW(h, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24))) {
                if (HGLOBAL g = LoadResource(h, r)) {
                    const auto* p = static_cast<const std::uint8_t*>(LockResource(g));
                    if (p) manifest = decode_manifest({p, SizeofResource(h, r)});
                }
            }
            FreeLibrary(h);
        }
    }
    if (manifest.empty()) read_text_file(exe_path + L".manifest", manifest);
    if (!manifest.empty()) load_sxs_from_manifest(manifest, app_dir);
}

} // namespace transmute::fx::loader
