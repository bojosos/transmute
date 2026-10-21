// Host-only tests of the UTType table (uttype.cpp). Split out of the QLab data
// group's tests (kit/completion/data/tests/data_tests.cpp).
#include "alchemy/darwin/coreservices/uttype.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

// ---- UTType ----
namespace uti = alchemy::gui::completion_kit::uttype;

void uniform_types() {
    require(uti::conforms("public.png", "public.image") && uti::conforms("public.png", "public.data") && !uti::conforms("public.png", "public.audio"), "conformance");
    require(uti::conforms("public.midi-audio", "public.audiovisual-content") && uti::conforms("com.apple.application-bundle", "com.apple.package"), "deep conformance");
    require(uti::conforms("PUBLIC.PNG", "public.image"), "identifiers are case-insensitive");
    require(uti::for_tag(uti::Tag::filename_extension, "JPG") == std::optional<std::string>("public.jpeg"), "extension lookup");
    require(uti::for_tag(uti::Tag::mime_type, "audio/midi") == std::optional<std::string>("public.midi-audio"), "MIME lookup");
    require(uti::for_tag(uti::Tag::filename_extension, "qlab5") == std::optional<std::string>("dyn.alchemy-ext-qlab5"), "dynamic type");
    require(!uti::for_tag(uti::Tag::filename_extension, "qlab5", "public.image"), "a dynamic type conforms only to public.data");
    require(uti::for_tag(uti::Tag::filename_extension, "xml", "public.text") == std::optional<std::string>("public.xml"), "conforming lookup");
    require(uti::conforms("dyn.alchemy-ext-qlab5", "public.data") && uti::is_dynamic("dyn.alchemy-ext-qlab5"), "dynamic conformance");
    require(uti::preferred_tag("public.jpeg", uti::Tag::filename_extension) == std::optional<std::string>("jpeg"), "preferred extension");
    require(uti::for_tag(uti::Tag::ostype, "APPL") == std::optional<std::string>("com.apple.application-bundle"), "OSType lookup");
    const auto supers = uti::supertypes("public.png");
    require(std::find(supers.begin(), supers.end(), "public.item") != supers.end() && std::find(supers.begin(), supers.end(), "public.png") == supers.end(), "supertypes");
}


struct Test { const char* name; std::function<void()> run; };

} // namespace

int main() {
    const std::vector<Test> tests = {
        {"uniform types", uniform_types},
    };
    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "ok   " << test.name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << test.name << ": " << e.what() << '\n';
        }
    }
    return failed ? 1 : 0;
}
