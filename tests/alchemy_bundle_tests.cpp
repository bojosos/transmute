#include "alchemy/bundle.hpp"
#include "alchemy/inspection.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void text_file(const std::filesystem::path& path, std::string_view value) {
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    require(bool(output), "fixture write failed");
}
std::vector<std::uint8_t> binary_plist() {
    std::vector<std::uint8_t> bytes{'b', 'p', 'l', 'i', 's', 't', '0', '0', 0xd1, 1, 2};
    const auto key_offset = bytes.size();
    constexpr std::string_view key = "CFBundleExecutable";
    bytes.insert(bytes.end(), {0x5f, 0x10, static_cast<std::uint8_t>(key.size())});
    bytes.insert(bytes.end(), key.begin(), key.end());
    const auto value_offset = bytes.size();
    bytes.push_back(0x57);
    constexpr std::string_view name = "Program";
    bytes.insert(bytes.end(), name.begin(), name.end());
    const auto offsets = bytes.size();
    bytes.insert(bytes.end(), {8, static_cast<std::uint8_t>(key_offset), static_cast<std::uint8_t>(value_offset)});
    const auto trailer = bytes.size();
    bytes.resize(trailer + 32);
    bytes[trailer + 6] = 1; bytes[trailer + 7] = 1;
    bytes[trailer + 15] = 3; bytes[trailer + 31] = static_cast<std::uint8_t>(offsets);
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected hello fixture and temporary directory");
        const auto root = std::filesystem::path(argv[2]) / "Fixture.app";
        std::filesystem::create_directories(root / "Contents" / "MacOS");
        const auto executable = root / "Contents" / "MacOS" / "Program";
        std::filesystem::copy_file(argv[1], executable, std::filesystem::copy_options::overwrite_existing);
        const auto plist = root / "Contents" / "Info.plist";
        text_file(plist, "<plist><dict><key>CFBundleExecutable</key><string>Program</string></dict></plist>");
        require(alchemy::discover_program(root) == std::filesystem::canonical(executable), "XML bundle discovery failed");
        require(alchemy::bundle_macho_files(root).size() == 1, "bundle inventory failed");
        for (bool jit : {false, true}) {
            std::string output;
            alchemy::RunOptions options; options.jit = jit; options.jit_stats = jit;
            const auto result = alchemy::run_program(root, {}, options, [&](auto, auto bytes) {
                output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                return alchemy::OutputResult{bytes.size(), 0};
            });
            require(result.exit_code == 0 && output == "Hello World from MacOS: 27\n", "bundle execution failed");
            if (jit) require(result.jit.native_guest_instructions != 0, "bundle JIT did not execute");
        }
        auto bytes = binary_plist();
        text_file(plist, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        require(alchemy::discover_program(root) == std::filesystem::canonical(executable), "binary bundle discovery failed");
        std::ostringstream report;
        require(alchemy::inspect_program(root, {}, true, true, true, report) == 0, "bundle inspection failed");
        require(report.str().find("\"schema\":1") != std::string::npos &&
                report.str().find("\"status\":\"provider\"") != std::string::npos, "JSON inspection incomplete");
        bytes[9] = 0xff;
        text_file(plist, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        bool rejected = false;
        try { static_cast<void>(alchemy::discover_program(root)); } catch (const std::exception&) { rejected = true; }
        require(rejected, "invalid binary plist reference accepted");
        text_file(plist, "<plist><key>CFBundleExecutable</key><string>../escape</string></plist>");
        rejected = false;
        try { static_cast<void>(alchemy::discover_program(root)); } catch (const std::exception&) { rejected = true; }
        require(rejected, "escaping bundle name accepted");
        text_file(plist, "<plist><dict><key>CFBundleExecutable</key><string>Program</string></dict></plist>");
        std::cout << "Alchemy bundle tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
