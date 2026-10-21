// Sampling profiler: TRANSMUTE_FX_SAMPLE syntax, atomic report replacement,
// and the measurement-window API (fx::profiler_reset / profiler_set_enabled).

#include "fx/runtime/profiler_window.hpp"
#include "fx/runtime/runtime_internal.hpp"
#include "fx/testing/test.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace transmute::fx;

namespace {

std::wstring temp_directory(const wchar_t* tag) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"fx-profiler-" + tag + L"-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::string read_text(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_text(const std::wstring& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

unsigned count_files(const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    unsigned n = 0;
    do ++n; while (FindNextFileW(find, &fd));
    FindClose(find);
    return n;
}

void spin_ms(unsigned ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    volatile std::uint64_t x = 0;
    while (GetTickCount64() < end) x = x + 1;
}

} // namespace

FX_TEST(profiler_sample_setting_syntax) {
    rt::SampleWindow w;
    FX_CHECK(!rt::parse_sample_setting(L"", w));
    FX_CHECK(!rt::parse_sample_setting(L"0", w));
    FX_REQUIRE(rt::parse_sample_setting(L"C:\\p.txt", w));
    FX_CHECK(w.path == L"C:\\p.txt");
    FX_CHECK_EQ(w.interval_us, 1000u);
    FX_CHECK(!w.main_only && !w.stacks && !w.background && !w.paused);
    FX_CHECK_EQ(w.delay_seconds, 0.0);
    FX_REQUIRE(rt::parse_sample_setting(L"1,2000,main,stacks,bg", w));
    FX_CHECK(w.path == L"1");
    FX_CHECK_EQ(w.interval_us, 2000u);
    FX_CHECK(w.main_only && w.stacks && w.background);
    FX_REQUIRE(rt::parse_sample_setting(L"p.txt,main,delay=12.5,duration=60", w));
    FX_CHECK_EQ(w.interval_us, 1000u);   // flags without an interval keep the default
    FX_CHECK(w.main_only);
    FX_CHECK_EQ(w.delay_seconds, 12.5);
    FX_CHECK_EQ(w.duration_seconds, 60.0);
    FX_REQUIRE(rt::parse_sample_setting(L"p.txt,50,paused", w));
    FX_CHECK_EQ(w.interval_us, 100u);    // clamped
    FX_CHECK(w.paused);
}

FX_TEST(profiler_report_replacement_is_atomic) {
    const std::wstring dir = temp_directory(L"atomic");
    const std::wstring target = dir + L"\\report.txt", temp = dir + L"\\report.txt.tmp1";
    write_text(target, "old report\n");
    write_text(temp, "new report\n");
    // A reader that allows delete sharing (as fx tools and most editors do)
    // keeps reading the old file while it is replaced.
    HANDLE reader = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    FX_CHECK(reader != INVALID_HANDLE_VALUE);
    FX_CHECK(rt::replace_file_atomically(temp, target));
    char buffer[32]{};
    DWORD got = 0;
    ReadFile(reader, buffer, sizeof(buffer) - 1, &got, nullptr);
    CloseHandle(reader);
    FX_CHECK(std::string(buffer) == "old report\n");
    FX_CHECK(read_text(target) == "new report\n");
    FX_CHECK_EQ(count_files(dir + L"\\*.tmp*"), 0u);
    // A reader without delete sharing blocks the rename: the old report stays whole.
    write_text(temp, "newer report\n");
    reader = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FX_CHECK(!rt::replace_file_atomically(temp, target));
    CloseHandle(reader);
    FX_CHECK(read_text(target) == "new report\n");
    DeleteFileW(temp.c_str());
    DeleteFileW(target.c_str());
    RemoveDirectoryW(dir.c_str());
}

// Starts the process's one profiler (paused, main thread at 200 us), opens a
// window, resets it, samples a second window and checks the final report.
FX_TEST(profiler_window_reset_and_enable) {
    rt::ensure_initialized();
    const std::wstring dir = temp_directory(L"window");
    const std::wstring report = dir + L"\\profile.txt";
    SetEnvironmentVariableW(L"TRANSMUTE_FX_SAMPLE", (report + L",200,main,paused").c_str());
    rt::profiler_start_if_requested();
    SetEnvironmentVariableW(L"TRANSMUTE_FX_SAMPLE", nullptr);
    spin_ms(100);                       // paused: nothing sampled
    profiler_set_enabled(true);
    spin_ms(300);
    profiler_reset();                   // forget the first 300 ms
    spin_ms(200);
    profiler_set_enabled(false);
    spin_ms(100);                       // paused again
    rt::profiler_flush();
    const std::string text = read_text(report);
    unsigned long long samples = 0, ticks = 0;
    FX_REQUIRE(std::sscanf(text.c_str(), "fx sampling profile: %llu samples, %llu ticks", &samples, &ticks) == 2);
    // ~200 ms at 200 us (timer resolution and suspension cost make it fewer),
    // never the 300 ms before the reset (1500 at full rate).
    FX_CHECK(samples > 20);
    FX_CHECK(samples < 1200);
    FX_CHECK(text.find("1 reset(s)") != std::string::npos);
    FX_CHECK(text.find("paused") != std::string::npos);
    FX_CHECK_EQ(count_files(dir + L"\\*.tmp*"), 0u);
    // Stopped for good: the window API is a no-op now.
    profiler_set_enabled(true);
    profiler_reset();
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}
