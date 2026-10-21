// alchemy install: Alchemy root layout, link recreation without privilege,
// manifest-backed path resolution and guest /Applications mapping.

#include "alchemy/alchemy_root.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/path.hpp"
#include "alchemy/process.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";

void require(bool value, std::string_view description) {
    if (!value) throw std::runtime_error(std::string(description));
}

void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string generic(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool reparse(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path));
#endif
}

struct Scratch {
    std::filesystem::path base;
    Scratch() {
        base = std::filesystem::temp_directory_path() / ("alchemy-install-tests-" + std::to_string(
#ifdef _WIN32
            GetCurrentProcessId()
#else
            0
#endif
        ));
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base);
    }
    ~Scratch() {
        alchemy::clear_alchemy_root();
        std::error_code error;
        // Junctions are removed as links by remove_all; their targets live in the same tree.
        std::filesystem::remove_all(base, error);
    }
};

// A 7-Zip-style extraction: framework links are empty regular files.
std::filesystem::path make_extracted_app(const std::filesystem::path& base, std::string_view name = "Demo.app") {
    const auto app = base / "source" / std::filesystem::path(name);
    write(app / "Contents" / "Info.plist", "<plist><dict><key>CFBundleExecutable</key><string>Demo</string></dict></plist>");
    write(app / "Contents" / "MacOS" / "Demo", "main-binary");
    const auto framework = app / "Contents" / "Frameworks" / "Kit.framework";
    write(framework / "Versions" / "B" / "Kit", "kit-binary-v-b");
    write(framework / "Versions" / "B" / "Resources" / "Info.plist", "kit-info");
    write(framework / "Versions" / "B" / "Helper.app" / "Contents" / "MacOS" / "Helper", "helper-binary");
    write(framework / "Versions" / "Current", "");
    write(framework / "Kit", "");
    write(framework / "Resources", "");
    write(framework / "Helper.app", "");
    write(app / "Contents" / "Resources" / "empty.txt", "");  // a genuine empty file stays a file
    return app;
}

void inferred_install_and_resolution() {
    Scratch scratch;
    const auto app = make_extracted_app(scratch.base);
    const auto root = scratch.base / "root";
    std::ostringstream log;
    alchemy::InstallOptions options{app, root, std::nullopt, true, false, false};
    const auto result = alchemy::install_app(options, log);
    require(result.links.size() == 4, "expected four framework links");
    require(result.inferred.size() == 4, "all four links should be inferred");
    const auto installed = std::filesystem::canonical(root) / "Applications" / "Demo.app";
    const auto framework = installed / "Contents" / "Frameworks" / "Kit.framework";
    require(std::filesystem::is_regular_file(std::filesystem::canonical(root) / ".alchemy-root"), "root marker missing");
    require(read(framework / "Kit") == "kit-binary-v-b", "Kit.framework/Kit does not reach Versions/B/Kit");
    require(read(framework / "Versions" / "Current" / "Kit") == "kit-binary-v-b", "Versions/Current does not reach B");
    require(read(framework / "Resources" / "Info.plist") == "kit-info", "Resources link broken");
    require(read(framework / "Helper.app" / "Contents" / "MacOS" / "Helper") == "helper-binary", "Helper.app link broken");
    require(reparse(framework / "Versions" / "Current"), "directory link must be a junction or symlink");
    require(std::filesystem::is_regular_file(installed / "Contents" / "Resources" / "empty.txt") &&
            std::filesystem::file_size(installed / "Contents" / "Resources" / "empty.txt") == 0, "genuine empty file changed");
    for (const auto& link : result.links) {
        require(link.representation == "junction" || link.representation == "hardlink" || link.representation == "copy",
                "native symlinks were disabled");
    }
    require(log.str().find("4 inferred") != std::string::npos, "install log lacks inference count");

    // Manifest-backed resolution (the loader's and doctor's realpath).
    alchemy::configure_alchemy_root(root);
    require(alchemy::installed_link_count() == 4, "manifest not loaded");
    const auto real = alchemy::installed_real_path(framework / "Kit");
    require(generic(real) == generic(std::filesystem::canonical(framework / "Versions" / "B" / "Kit")),
            "Kit.framework/Kit must resolve to Versions/B/Kit, got " + generic(real));
    const auto via_current = alchemy::installed_real_path(framework / "Versions" / "Current" / "Kit");
    require(generic(via_current) == generic(real), "Versions/Current/Kit must resolve to the same image");
    const auto link = alchemy::installed_link(framework / "Kit");
    require(link && link->target == "Versions/Current/Kit" && !link->directory, "Kit link record wrong");
    const auto current = alchemy::installed_link(framework / "Versions" / "Current");
    require(current && current->target == "B" && current->directory, "Versions/Current record wrong");
    require(!alchemy::installed_link(framework / "Versions" / "B" / "Kit"), "real file reported as a link");

    // Guest path mapping both ways.
    require(alchemy::guest_path_string(framework / "Kit") == "/Applications/Demo.app/Contents/Frameworks/Kit.framework/Kit",
            "host path did not map to /Applications: " + alchemy::guest_path_string(framework / "Kit"));
    const auto host = alchemy::guest_root_host_path("/Applications/Demo.app/Contents/MacOS/Demo");
    require(host && read(*host) == "main-binary", "/Applications did not map into the root");
    require(!alchemy::guest_root_host_path("/ApplicationsX/Demo"), "partial component matched");
    require(alchemy::guest_root_host_path("/Library/Preferences").has_value(), "/Library not mapped");
    require(alchemy::find_alchemy_root(framework / "Kit") &&
            generic(*alchemy::find_alchemy_root(framework / "Kit")) == generic(std::filesystem::canonical(root)),
            "root marker discovery failed");

    // Guest services: readlink/lstat report the macOS link text; open follows it.
    {
        transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
        alchemy::ProcessContext context{engine, [](unsigned, auto bytes) { return alchemy::OutputResult{bytes.size(), 0}; }};
        alchemy::DarwinServices services{context};
        context.working_directory = std::filesystem::current_path();
        auto& memory = static_cast<transmute::GuestMemory&>(engine);
        const auto call = [&](std::string_view name, std::initializer_list<std::uint64_t> args) {
            const auto symbol = context.resolve(system_library, name);
            require(symbol.has_value(), "service missing");
            return engine.invoke_callback(symbol->address, std::span(args.begin(), args.size()));
        };
        const auto text = [&](std::string_view value) {
            const auto address = context.allocate_guest(value.size() + 1);
            memory.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
            memory.write8(address + value.size(), 0);
            return address;
        };
        const auto buffer = context.allocate_guest(1024);
        const auto guest_kit = text("/Applications/Demo.app/Contents/Frameworks/Kit.framework/Kit");
        const auto count = call("_readlink", {guest_kit, buffer, 1024});
        memory.write8(buffer + count, 0); const auto bytes = memory.read_string(buffer, 1, 1024);
        require(std::string(bytes.begin(), bytes.end()) == "Versions/Current/Kit", "guest readlink lost the link text");
        const auto current_count = call("_readlink", {text("/Applications/Demo.app/Contents/Frameworks/Kit.framework/Versions/Current"), buffer, 1024});
        memory.write8(buffer + current_count, 0); const auto current_bytes = memory.read_string(buffer, 1, 1024);
        require(std::string(current_bytes.begin(), current_bytes.end()) == "B", "guest readlink of a junction lost the link text");
        const auto metadata = context.allocate_guest(144);
        require(call("_lstat", {guest_kit, metadata}) == 0, "guest lstat failed");
        require((memory.read8(metadata + 5) & 0xf0) == 0xa0, "lstat did not report S_IFLNK");
        require(engine.read64(metadata + 96) == std::string_view("Versions/Current/Kit").size(), "lstat size is not the link length");
        const auto resolved = call("_realpath", {guest_kit, 0});
        const auto real_bytes = memory.read_string(resolved, 1, 4096);
        require(std::string(real_bytes.begin(), real_bytes.end()) == "/Applications/Demo.app/Contents/Frameworks/Kit.framework/Versions/B/Kit",
                "guest realpath did not follow the recorded link");
    }

    // A second install needs --replace; replacing keeps link targets intact.
    bool refused = false;
    try { std::ostringstream ignored; static_cast<void>(alchemy::install_app(options, ignored)); }
    catch (const std::runtime_error& error) { refused = std::string(error.what()).find("--replace") != std::string::npos; }
    require(refused, "reinstall without --replace must fail");
    options.replace = true;
    std::ostringstream replaced;
    static_cast<void>(alchemy::install_app(options, replaced));
    require(read(framework / "Kit") == "kit-binary-v-b", "replace broke the installation");
    require(read(app / "Contents" / "Frameworks" / "Kit.framework" / "Versions" / "B" / "Kit") == "kit-binary-v-b",
            "replace damaged the source");
}

void link_list_install() {
    Scratch scratch;
    const auto app = make_extracted_app(scratch.base, "Listed.app");
    const auto list = scratch.base / "links.tsv";
    write(list, "# recovered from the disk image\n"
                "Contents/Frameworks/Kit.framework/Versions/Current\tB\n"
                "Contents/Frameworks/Kit.framework/Kit\tVersions/Current/Kit\n"
                "Contents/Frameworks/Kit.framework/Resources\tVersions/Current/Resources\n"
                "Contents/Frameworks/Kit.framework/Helper.app\tVersions/Current/Helper.app\n");
    std::ostringstream log;
    const auto result = alchemy::install_app({app, scratch.base / "root", list, true, false, false}, log);
    require(result.links.size() == 4 && result.inferred.empty(), "link list must supply every link");

    // Escaping, absolute and dangling targets fail before anything is installed.
    for (const auto* bad : {"Contents/Frameworks/Kit.framework/Kit\t../../../../../outside\n",
                            "Contents/Frameworks/Kit.framework/Kit\t/Applications\n",
                            "Contents/Frameworks/Kit.framework/Kit\tVersions/Current/Missing\n"}) {
        write(list, bad);
        bool failed = false;
        try { std::ostringstream ignored; static_cast<void>(alchemy::install_app({app, scratch.base / "root2", list, false, true, false}, ignored)); }
        catch (const std::runtime_error&) { failed = true; }
        require(failed, std::string("invalid link accepted: ") + bad);
        require(!std::filesystem::exists(scratch.base / "root2" / "Applications" / "Listed.app"), "failed install left files");
    }

    // Ambiguous framework versions cannot be inferred.
    const auto ambiguous = make_extracted_app(scratch.base / "two", "Two.app");
    write(ambiguous / "Contents" / "Frameworks" / "Kit.framework" / "Versions" / "A" / "Kit", "old");
    bool failed = false;
    try { std::ostringstream ignored; static_cast<void>(alchemy::install_app({ambiguous, scratch.base / "root3", std::nullopt, true, false, false}, ignored)); }
    catch (const std::runtime_error& error) { failed = std::string(error.what()).find("--links") != std::string::npos; }
    require(failed, "ambiguous Versions/Current inference must fail explicitly");
}

// Run-time links (guest symlink(2) without host privilege) and the system
// directories mapped into a root (installers write /Users, /etc, /var).
void runtime_links_and_system_directories() {
    Scratch scratch;
    const auto root = alchemy::create_alchemy_root(scratch.base / "root");
    for (const auto* directory : {"Users", "etc", "usr/local", "var/log", ".alchemy/Volumes"})
        require(std::filesystem::is_directory(root / directory), std::string("root layout lacks ") + directory);
    alchemy::configure_alchemy_root(root);
    require(alchemy::guest_root_host_path("/Users/alchemy/Library") == alchemy::utf8_path(generic(root) + "/Users/alchemy/Library"),
            "/Users is not mapped into the root");
    require(alchemy::guest_root_host_path("/etc/hosts").has_value() && alchemy::guest_root_host_path("/var/log/x").has_value(),
            "/etc or /var not mapped");
    require(!alchemy::guest_root_host_path("/System/Library/Frameworks"), "/System must not map into the root");
    require(alchemy::guest_path_string(root / "Users" / "alchemy") == "/Users/alchemy", "reverse /Users mapping");

    const auto product = root / "Users" / "alchemy" / "Product";
    write(product / "bin" / "tool", "tool-binary");
    std::filesystem::create_directories(product / "data" / "inner");
    const auto file_link = product / "Tool";
    const auto directory_link = product / "Resources";
    alchemy::create_runtime_link(file_link, "bin/tool");
    alchemy::create_runtime_link(directory_link, "data");
    require(read(file_link) == "tool-binary", "file link does not show the target");
    require(std::filesystem::is_directory(directory_link / "inner"), "directory link does not show the target");
    require(alchemy::installed_link(file_link) && alchemy::installed_link(file_link)->target == "bin/tool", "file link not recorded");
    require(alchemy::installed_link(directory_link) && alchemy::installed_link(directory_link)->directory, "directory link not recorded");
    const auto manifest = read(root / ".alchemy" / "links" / "runtime.tsv");
    require(manifest.find("Users/alchemy/Product/Tool\tbin/tool") != std::string::npos, "runtime manifest lacks the file link");
    bool refused = false;
    try { alchemy::create_runtime_link(product / "Dangling", "missing/file"); } catch (const std::system_error&) { refused = true; }
    require(refused && !std::filesystem::exists(product / "Dangling"), "a dangling run-time link was created");
    refused = false;
    try { alchemy::create_runtime_link(file_link, "bin/tool"); } catch (const std::system_error&) { refused = true; }
    require(refused, "an existing link was replaced");
    // Records survive reconfiguration (as a new process would load them).
    alchemy::configure_alchemy_root(root);
    require(alchemy::installed_link(directory_link).has_value(), "runtime.tsv was not loaded");
    std::filesystem::remove(file_link);
    alchemy::forget_installed_link(file_link);
    require(!alchemy::installed_link(file_link), "forgotten link still recorded");
    require(read(root / ".alchemy" / "links" / "runtime.tsv").find("Product/Tool\t") == std::string::npos, "runtime manifest kept a removed link");
    require(read(product / "bin" / "tool") == "tool-binary", "removing the link changed its target");
}

void unconfigured_paths_unchanged() {
    alchemy::clear_alchemy_root();
    require(!alchemy::guest_root_host_path("/Applications/X.app"), "unconfigured root mapped /Applications");
#ifdef _WIN32
    require(alchemy::guest_path_string(std::filesystem::path("C:/Users/x")) == "/Volumes/C/Users/x", "drive mapping changed");
#endif
    const auto sample = std::filesystem::temp_directory_path();
    require(alchemy::resolve_installed_links(sample) == sample, "unconfigured resolution changed a path");
}

} // namespace

int main() {
    try {
        unconfigured_paths_unchanged();
        inferred_install_and_resolution();
        link_list_install();
        runtime_links_and_system_directories();
        unconfigured_paths_unchanged();
        std::cout << "Alchemy install tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
