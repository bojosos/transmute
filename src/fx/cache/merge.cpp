// Main-file publication and journal merging (owner: cache).
//
// Writers of <stem>.fxc hold <stem>.lock (exclusive, delete-on-close), write
// <stem>.t<pid>.tmp and rename it over the main file. Readers map main files
// with FILE_SHARE_DELETE, so a rename never disturbs a running process: it
// keeps its old mapping, new processes see the new file. A journal can be
// merged only once its writer has closed it (opening it without write
// sharing fails while the owner is alive); merged journals are deleted.

#include "fx/cache/cache_internal.hpp"
#include "fx/cache/format.hpp"
#include "fx/cache/pe_view.hpp"
#include "fx/host/file.hpp"
#include "fx/host/process.hpp"
#include "fx/host/time.hpp"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace transmute::fx::cache_detail {
namespace {

using cachefmt::FileHeader;
using cachefmt::Key;
using cachefmt::RecordRef;
using cachefmt::RecordView;

namespace file = host::file;

constexpr std::uint64_t kSecond = 10'000'000ULL;  // host::wall_time() units (FILETIME)
constexpr std::uint64_t kStaleKeyAge = 7ULL * 24 * 3600 * kSecond;
constexpr std::uint64_t kStaleTempAge = 3600ULL * kSecond;

std::wstring hex16(std::uint64_t value) {
    static constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring s(16, L'0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = kDigits[value & 15];
        value >>= 4;
    }
    return s;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring directory_of(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

std::uint64_t now_filetime() { return host::wall_time(); }

class HandleSink final : public cachefmt::FileSink {
public:
    explicit HandleSink(file::File handle) : handle_(handle) { buffer_.reserve(kBuffer); }
    bool write(const void* data, std::size_t size) override {
        if (!ok_) return false;
        const auto* p = static_cast<const std::uint8_t*>(data);
        if (buffer_.size() + size > kBuffer && !flush()) return false;
        if (size >= kBuffer) return ok_ = raw(p, size);
        buffer_.insert(buffer_.end(), p, p + size);
        return true;
    }
    bool write_at(std::uint64_t offset, const void* data, std::size_t size) override {
        if (!flush()) return false;
        if (!file::seek(handle_, offset)) return ok_ = false;
        ok_ = raw(static_cast<const std::uint8_t*>(data), size);
        file::seek_end(handle_);
        return ok_;
    }
    bool flush() {
        if (ok_ && !buffer_.empty()) {
            ok_ = raw(buffer_.data(), buffer_.size());
            buffer_.clear();
        }
        return ok_;
    }

private:
    static constexpr std::size_t kBuffer = 1u << 20;
    bool raw(const std::uint8_t* p, std::size_t size) { return file::write_all(handle_, p, size); }
    file::File handle_;
    std::vector<std::uint8_t> buffer_;
    bool ok_ = true;
};

file::File try_lock(const std::wstring& stem) {
    return file::open(stem + L".lock", file::kRead | file::kWrite, file::kShareNone, file::Create::open_always,
                      file::Flags::lock_file);
}

file::File lock_with_wait(const std::wstring& stem, unsigned wait_ms) {
    const std::uint64_t deadline = host::monotonic_ms() + wait_ms;
    for (;;) {
        file::File lock = try_lock(stem);
        if (lock || host::monotonic_ms() >= deadline) return lock;
        host::sleep_ms(10);
    }
}

// <stem>.lock held.
bool publish_main_locked(const std::wstring& stem, const Key& key, std::uint64_t generation,
                         std::span<const RecordRef> records, cachefmt::BuildResult* result, std::wstring* error,
                         const wchar_t* extension = L".fxc") {
    const std::wstring temp = stem + L".t" + hex16(host::process_id()).substr(8) + L".tmp";
    const std::wstring target = stem + extension;
    file::File out = file::open(temp, file::kWrite, file::kShareNone, file::Create::create_always,
                                file::Flags::sequential);
    if (!out) {
        if (error) *error = L"cannot create " + temp;
        return false;
    }
    HandleSink sink(out);
    const bool written = cachefmt::build_main(key, generation, records, sink, result) && sink.flush();
    file::close(out);
    if (!written) {
        file::remove(temp);
        if (error) *error = L"cannot write " + temp;
        return false;
    }
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (file::rename_replace(temp, target)) return true;
        host::sleep_ms(2);
    }
    file::remove(temp);
    if (error) *error = L"cannot replace " + target;
    return false;
}

void delete_matching(const std::wstring& pattern, const std::wstring& keep_lower, std::uint64_t min_age) {
    const std::wstring directory = directory_of(pattern);
    const std::uint64_t now = now_filetime();
    for (const file::Entry& entry : file::list(pattern)) {
        if (entry.directory) continue;
        const std::wstring name = lower(entry.name);
        if (!keep_lower.empty() && name.rfind(keep_lower, 0) == 0) continue;
        const std::uint64_t written = entry.write_time;
        if (now > written && now - written >= min_age) file::remove(directory + L"\\" + entry.name);
    }
}

// Removes files of the same module under other keys (old translator ABI,
// other options, older image versions) once unused for a week, and stale
// temporary files left by crashed writers.
void collect_garbage(const std::wstring& stem) {
    const std::wstring directory = directory_of(stem);
    const std::wstring file = stem.substr(directory.size() + 1);
    const std::size_t dot = file.rfind(L'.');
    if (dot == std::wstring::npos) return;
    const std::wstring module = file.substr(0, dot);
    const std::wstring own = lower(file) + L".";
    delete_matching(directory + L"\\" + module + L".*.fxc", own, kStaleKeyAge);
    delete_matching(directory + L"\\" + module + L".*.fxj", own, kStaleKeyAge);
    delete_matching(directory + L"\\" + module + L".*.fxa", own, kStaleKeyAge);
    delete_matching(directory + L"\\" + module + L".*.tmp", std::wstring(), kStaleTempAge);
}

struct Journal {
    file::File handle = nullptr;
    const std::uint8_t* view = nullptr;
    std::size_t size = 0;
    std::uint64_t time = 0;
    bool valid = false;
};

void close_journal(Journal& j, bool remove) {
    if (j.view) file::unmap(j.view);
    j.view = nullptr;
    if (j.handle) {
        // Removes the name even while other processes map the journal.
        if (remove) file::mark_deleted(j.handle);
        file::close(j.handle);
        j.handle = nullptr;
    }
}

const std::uint8_t* map_handle(file::File handle, std::size_t& size) {
    std::uint64_t length = 0;
    if (!file::size(handle, length) || length == 0) return nullptr;
    const std::uint8_t* view = file::map_read_only(handle);
    if (!view) return nullptr;
    size = static_cast<std::size_t>(length);
    return view;
}

void collect_main_records(const cachefmt::MainView& main, std::vector<RecordRef>& refs) {
    const std::size_t begin = static_cast<std::size_t>(main.records_offset);
    const std::size_t end = static_cast<std::size_t>(main.records_offset + main.records_size);
    cachefmt::walk_records(main.data, begin, end, true, [&](std::size_t offset, const RecordView& view) {
        refs.push_back({main.data + offset, view.header->size});
    });
}

} // namespace

bool ensure_directory(const std::wstring& path) {
    const file::Directory made = file::create_directory(path);
    if (made == file::Directory::created || made == file::Directory::exists) return true;
    if (made != file::Directory::parent_missing) return false;
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash < 3) return false;
    if (!ensure_directory(path.substr(0, slash))) return false;
    const file::Directory again = file::create_directory(path);
    return again == file::Directory::created || again == file::Directory::exists;
}

bool write_main(const std::wstring& stem, const Key& key, std::span<const RecordRef> records, bool include_existing,
                cachefmt::BuildResult* result, std::wstring* error, const wchar_t* extension) {
    if (!ensure_directory(directory_of(stem))) {
        if (error) *error = L"cannot create cache directory " + directory_of(stem);
        return false;
    }
    file::File lock = lock_with_wait(stem, 30'000);
    if (!lock) {
        if (error) *error = L"cache file is locked: " + stem + L".lock";
        return false;
    }
    std::vector<RecordRef> refs;
    pe::MappedFile main_file;
    std::uint64_t generation = 1;
    cachefmt::MainView main;
    if (include_existing && main_file.open(stem + extension, false) &&
        cachefmt::open_main(main_file.data(), main_file.size(), key, main)) {
        collect_main_records(main, refs);
        generation = main.header->generation + 1;
    }
    refs.insert(refs.end(), records.begin(), records.end());
    const bool ok = publish_main_locked(stem, key, generation, refs, result, error, extension);
    main_file.close();
    file::close(lock);
    if (ok) collect_garbage(stem);
    return ok;
}

bool merge_stem(const std::wstring& stem, bool force, bool settled) {
    file::File lock = try_lock(stem);
    if (!lock) return false;  // another process is merging
    const std::wstring directory = directory_of(stem);
    const std::wstring digest = lower(stem.substr(stem.rfind(L'.') + 1));

    std::vector<Journal> journals;
    for (const file::Entry& entry : file::list(stem + L".j*.fxj")) {
        if (entry.directory) continue;
        Journal j;
        // No write sharing: fails while the owning process still has it open.
        j.handle = file::open(directory + L"\\" + entry.name, file::kRead | file::kDelete, file::kRead | file::kDelete,
                              file::Create::open_existing, file::Flags::normal);
        if (!j.handle) continue;
        j.time = entry.write_time;
        j.view = map_handle(j.handle, j.size);
        journals.push_back(j);
    }
    auto finish = [&](bool remove_journals, bool result) {
        for (Journal& j : journals) close_journal(j, remove_journals || !j.valid);
        file::close(lock);
        return result;
    };
    if (journals.empty()) return finish(false, false);

    pe::MappedFile main_file;
    FileHeader main_header{};
    cachefmt::MainView main;
    bool has_main = main_file.open(stem + L".fxc", false) &&
                    cachefmt::parse_header(main_file.data(), main_file.size(), cachefmt::kKindMain, main_header) &&
                    hex16(main_header.key.digest()) == digest &&
                    cachefmt::open_main(main_file.data(), main_file.size(), main_header.key, main);
    Key key = main_header.key;
    bool have_key = has_main;
    std::uint64_t journal_bytes = 0;
    for (Journal& j : journals) {
        FileHeader h;
        j.valid = j.view && cachefmt::parse_header(j.view, j.size, cachefmt::kKindJournal, h) &&
                  hex16(h.key.digest()) == digest && (!have_key || h.key == key);
        if (j.valid && !have_key) {
            key = h.key;
            have_key = true;
        }
        if (j.valid) journal_bytes += j.size - sizeof(FileHeader);
    }
    if (!have_key) return finish(true, false);  // nothing usable: drop the garbage
    if (!force && has_main && !(settled && main_file.size() <= kSettledRewriteBytes)) {
        const std::uint64_t threshold = std::max<std::uint64_t>(merge_min_bytes(), main_file.size() / 8);
        if (journal_bytes < threshold && journals.size() < 8) {
            for (Journal& j : journals) j.valid = true;  // keep them
            return finish(false, false);
        }
    }

    std::vector<RecordRef> refs;
    if (has_main) collect_main_records(main, refs);
    std::stable_sort(journals.begin(), journals.end(),
                     [](const Journal& a, const Journal& b) { return a.time < b.time; });
    for (const Journal& j : journals) {
        if (!j.valid) continue;
        cachefmt::walk_records(j.view, sizeof(FileHeader), j.size, true,
                               [&](std::size_t offset, const RecordView& view) {
                                   refs.push_back({j.view + offset, view.header->size});
                               });
    }
    const std::uint64_t generation = has_main ? main_header.generation + 1 : 1;
    const bool ok = publish_main_locked(stem, key, generation, refs, nullptr, nullptr);
    main_file.close();
    if (!ok) {
        for (Journal& j : journals) j.valid = true;  // keep for a later attempt
        return finish(false, false);
    }
    finish(true, true);
    collect_garbage(stem);
    return true;
}

} // namespace transmute::fx::cache_detail
