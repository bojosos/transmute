// MD4 against the RFC 1320 vectors (md4.hpp). Split out of the export-fill tests.
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"
#include "alchemy/darwin/security/md4.hpp"

namespace {

void test_md4() {
    const auto digest = [](std::string_view text) {
        vd::Md4 md;
        md.update(bytes(text));
        const auto value = md.finish();
        std::string hex;
        for (const auto byte : value) { static constexpr char d[] = "0123456789abcdef"; hex += d[byte >> 4]; hex += d[byte & 15]; }
        return hex;
    };
    // RFC 1320 appendix A.5.
    require(digest("") == "31d6cfe0d16ae931b73c59d7e0c089c0", "MD4 of empty string");
    require(digest("abc") == "a448017aaf21d8525fc10ae87aa6729d", "MD4 of abc");
    require(digest("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
            "e33b4ddc9c38f2199c3e7b164fcc0536", "MD4 of the 80-digit vector");
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("md4"); test_md4();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "md4 tests passed\n";
    return 0;
}
