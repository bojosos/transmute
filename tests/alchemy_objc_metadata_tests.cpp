#include "alchemy/objc_metadata.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
constexpr std::uint64_t base = 0x100000000ULL;
constexpr std::uint64_t code = base + 0x10000;
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
void rejects(const std::function<void()>& operation, std::string_view expected) {
    try { operation(); }
    catch (const std::runtime_error& error) {
        require(std::string_view(error.what()).find(expected) != std::string_view::npos,
            "metadata failure did not identify its unsupported requirement");
        return;
    }
    throw std::runtime_error("malformed Objective-C metadata was accepted");
}
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{code}};
    alchemy::ProcessContext process{engine, [](auto, auto bytes) -> alchemy::OutputResult { return {bytes.size(), 0}; }};
    alchemy::ImageRuntimeInfo image;
    Fixture() {
        engine.map_memory(base, 4096);
        engine.map_memory(code, 4096);
        engine.write32(code, 0xd65f03c0);
        engine.set_memory_access(code, true, false, true);
        image.module_id = 1;
        image.begin = base; image.end = code + 4096;
        image.code_ranges.push_back({code, code + 4096});
        image.sections.push_back({"__DATA", "__objc_classlist", base, 8, 0});
        image.sections.push_back({"__DATA", "__objc_selrefs", base + 8, 8, 0});
        image.sections.push_back({"__DATA", "__objc_imageinfo", base + 16, 8, 0});
        engine.write64(base, base + 64); engine.write64(base + 8, base + 512);
        engine.write64(base + 64, base + 104); engine.write64(base + 96, base + 160);
        engine.write64(base + 104, base + 104); engine.write64(base + 112, base + 64);
        engine.write64(base + 136, base + 240);
        ro(base + 160, 2, 8, 16); ro(base + 240, 3, 40, 40);
        text(base + 512, "value"); text(base + 528, "Q16@0:8"); text(base + 560, "Fixture");
        engine.write64(base + 192, base + 1024);
        engine.write32(base + 1024, 24); engine.write32(base + 1028, 1);
        engine.write64(base + 1032, base + 512); engine.write64(base + 1040, base + 528);
        engine.write64(base + 1048, code);
    }
    void text(std::uint64_t address, std::string_view text) {
        engine.write_bytes(address, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
        engine.fill_bytes(address + text.size(), 0, 1);
    }
    void ro(std::uint64_t address, std::uint32_t flags, std::uint32_t start, std::uint32_t size) {
        engine.write32(address, flags); engine.write32(address + 4, start); engine.write32(address + 8, size);
        engine.write64(address + 24, base + 560);
    }
    std::vector<alchemy::ObjcImageInfo> parse() {
        const std::array images{image};
        return alchemy::parse_objc_images(process, engine, images);
    }
    void relative(std::uint64_t field, std::uint64_t target) {
        engine.write32(field, static_cast<std::uint32_t>(target - field));
    }
};
void ordinary_and_small_methods() {
    Fixture f;
    auto decoded = f.parse();
    require(decoded.size() == 1 && decoded[0].classes.size() == 2, "class and metaclass were not decoded");
    const auto& cls = decoded[0].classes[0];
    require(cls.name == "Fixture" && cls.instance_size == 16 && cls.methods.size() == 1 &&
        cls.methods[0].selector == "value" && cls.methods[0].implementation == code, "ordinary method layout mismatch");
    require(decoded[0].selectors[0].slot == base + 8 && f.engine.read64(base + 8) == base + 512,
        "metadata parsing modified a selector reference");
    f.engine.write32(base + 1024, 0xc000000c);
    f.relative(base + 1032, base + 512); f.relative(base + 1036, base + 528); f.relative(base + 1040, code);
    decoded = f.parse();
    require(decoded[0].classes[0].methods[0].selector == "value", "direct small method selector mismatch");
    f.engine.write32(base + 1024, 0x8000000c);
    f.relative(base + 1032, base + 8);
    require(f.parse()[0].classes[0].methods[0].implementation == code, "indirect small method mismatch");
}
void ivars_categories_and_nonlazy_lists() {
    Fixture f;
    f.engine.write64(base + 208, base + 1200);
    f.engine.write32(base + 1200, 32); f.engine.write32(base + 1204, 1);
    f.engine.write64(base + 1208, base + 1400); f.engine.write32(base + 1400, 8);
    f.engine.write64(base + 1216, base + 580); f.text(base + 580, "count");
    f.engine.write64(base + 1224, base + 596); f.text(base + 596, "Q");
    f.engine.write32(base + 1232, 3); f.engine.write32(base + 1236, 8);
    f.image.sections.push_back({"__DATA", "__objc_catlist", base + 32, 8, 0});
    f.image.sections.push_back({"__DATA", "__objc_nlclslist", base + 40, 8, 0});
    f.image.sections.push_back({"__DATA", "__objc_nlcatlist", base + 48, 8, 0});
    f.engine.write64(base + 32, base + 1440); f.engine.write64(base + 40, base + 64);
    f.engine.write64(base + 48, base + 1440); f.engine.write64(base + 1440, base + 608);
    f.text(base + 608, "Extra"); f.engine.write64(base + 1448, base + 64);
    f.engine.write64(base + 1456, base + 1024);
    auto decoded = f.parse();
    require(decoded[0].classes[0].ivars[0].offset == 8 && decoded[0].classes[0].ivars[0].alignment == 8,
        "ivar offset or alignment mismatch");
    require(decoded[0].categories[0].name == "Extra" && decoded[0].nonlazy_classes.size() == 1 &&
        decoded[0].nonlazy_categories.size() == 1, "category/nonlazy metadata mismatch");
    f.engine.write32(base + 1400, 15);
    rejects([&] { static_cast<void>(f.parse()); }, "ivar outside");
}
void malformed_metadata_is_transactional() {
    Fixture f;
    f.engine.write32(base + 1028, UINT32_MAX);
    rejects([&] { static_cast<void>(f.parse()); }, "count exceeds");
    f.engine.write32(base + 1028, 1); f.engine.write64(base + 1048, base + 560);
    rejects([&] { static_cast<void>(f.parse()); }, "outside executable");
    f.engine.write64(base + 1048, code); f.engine.write64(base + 1032, 0x600000000ULL);
    rejects([&] { static_cast<void>(f.parse()); }, "outside loaded image");
    f.engine.write64(base + 1032, base + 512); f.engine.write32(base + 20, 0x1);
    rejects([&] { static_cast<void>(f.parse()); }, "unsupported image-info flags");
    // Swift images record their Swift ABI (bits 8-15) and language (16-31) versions.
    f.engine.write32(base + 20, 0x06020700);
    require(f.parse().size() == 1, "Swift image-info version bits rejected");
    // A weak ivar layout is descriptive (ARC code manages __weak ivars itself).
    f.engine.write32(base + 20, 0); f.engine.write64(base + 216, base + 608);
    require(f.parse().size() == 1, "weak ivar layout rejected");
    f.engine.write64(base + 216, 0); f.engine.write32(base + 1024, 16);
    rejects([&] { static_cast<void>(f.parse()); }, "entry size");
    f.engine.write32(base + 1024, 24);
    auto invalid = f.image; invalid.module_id = 2; invalid.sections[0].size = 7;
    const std::array batch{f.image, invalid};
    rejects([&] { static_cast<void>(alchemy::parse_objc_images(f.process, f.engine, batch)); }, "pointer aligned");
    require(f.engine.read64(base + 8) == base + 512 && f.process.images().empty(),
        "failed metadata batch mutated guest pointers or published an image");
    invalid.sections[0] = {"__DATA", "__objc_classlist", UINT64_MAX - 3, 8, 0};
    const std::array overflow{invalid};
    rejects([&] { static_cast<void>(alchemy::parse_objc_images(f.process, f.engine, overflow)); }, "overflow");
}
void protocols_and_references() {
    Fixture f;
    constexpr auto first = base + 2048, second = base + 2160;
    f.image.sections.push_back({"__DATA", "__objc_protolist", base + 56, 8, 0});
    f.image.sections.push_back({"__DATA", "__objc_protorefs", base + 2000, 8, 0});
    f.engine.write64(base + 56, second); f.engine.write64(base + 2000, first);
    f.text(base + 2400, "Base"); f.text(base + 2408, "Derived"); f.text(base + 2420, "Q16@0:8");
    f.engine.write64(first + 8, base + 2400); f.engine.write32(first + 64, 96);
    f.engine.write64(second + 8, base + 2408); f.engine.write32(second + 64, 96);
    f.engine.write64(second + 16, base + 2272); f.engine.write64(base + 2272, 1); f.engine.write64(base + 2280, first);
    f.engine.write64(second + 24, base + 2304);
    f.engine.write32(base + 2304, 24); f.engine.write32(base + 2308, 1);
    f.engine.write64(base + 2312, base + 512); f.engine.write64(base + 2320, base + 528);
    f.engine.write64(second + 72, base + 2344); f.engine.write64(base + 2344, base + 2420);
    f.engine.write64(base + 200, base + 2360); f.engine.write64(base + 2360, 1); f.engine.write64(base + 2368, second);
    auto decoded = f.parse();
    const auto& image = decoded[0];
    require(image.protocols.size() == 2 && image.protocols[0].address == second && image.protocols[0].listed &&
        image.protocols[1].address == first && !image.protocols[1].listed, "listed and referenced protocols were not decoded");
    const auto& derived = image.protocols[0];
    require(derived.name == "Derived" && derived.size == 96 && derived.protocols.size() == 1 &&
        derived.protocols[0].slot == base + 2280 && derived.protocols[0].protocol == first, "protocol inheritance mismatch");
    require(derived.instance_methods.size() == 1 && derived.instance_methods[0].selector == "value" &&
        !derived.instance_methods[0].implementation && derived.extended_types.size() == 1 &&
        derived.extended_types[0] == "Q16@0:8", "protocol method descriptions mismatch");
    require(image.classes[0].protocols.size() == 1 && image.classes[0].protocols[0].slot == base + 2368 &&
        image.protocol_references.size() == 1 && image.protocol_references[0].slot == base + 2000,
        "class protocol list or protocol reference mismatch");
    require(f.engine.read64(base + 2000) == first && !f.engine.read64(first), "protocol parsing mutated guest metadata");
    f.engine.write64(first + 16, base + 2288); f.engine.write64(base + 2288, 1); f.engine.write64(base + 2296, second);
    rejects([&] { static_cast<void>(f.parse()); }, "cyclic protocol inheritance");
    f.engine.write64(first + 16, 0); f.engine.write32(first + 68, 1U << 31);
    rejects([&] { static_cast<void>(f.parse()); }, "unsupported protocol flags");
    f.engine.write32(first + 68, 0); f.engine.write32(first + 64, 64);
    rejects([&] { static_cast<void>(f.parse()); }, "protocol structure size");
    f.engine.write32(first + 64, 96); f.engine.write64(base + 2368, base + 2401);
    rejects([&] { static_cast<void>(f.parse()); }, "invalid protocol reference");
    f.engine.write64(base + 2368, second);
    f.image.sections.push_back({"__DATA", "__objc_catlist2", base + 32, 8, 0});
    rejects([&] { static_cast<void>(f.parse()); }, "Swift stub categories");
}
} // namespace
int main() {
    try {
        ordinary_and_small_methods(); ivars_categories_and_nonlazy_lists(); malformed_metadata_is_transactional();
        protocols_and_references();
        std::cout << "Objective-C metadata tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
