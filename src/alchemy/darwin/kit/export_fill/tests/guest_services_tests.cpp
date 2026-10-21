// The daemon-path case still shared here (guest services: CommonCrypto,
// IOKit, CF and libSystem calls together). The other cases are in their
// frameworks' tests: src/alchemy/darwin/{compression,libsystem,dispatch,identities,
// security,corefoundation,foundation,coreservices}/tests/.
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"

namespace {

void test_guest_services(Fixture& f) {
    const auto cf = alchemy::core_foundation_library;
    const auto system = "/usr/lib/libSystem.B.dylib";
    // CommonCrypto: NIST SP 800-38A F.2.1 / F.1.1 (AES-128 CBC and ECB).
    const auto key = hex("2b7e151628aed2a6abf7158809cf4f3c"), iv = hex("000102030405060708090a0b0c0d0e0f");
    const auto plain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    const auto crypt = [&f, system](std::uint32_t op, std::uint32_t options, const std::vector<std::uint8_t>& key_bytes,
                                    const std::vector<std::uint8_t>* iv_bytes, const std::vector<std::uint8_t>& input,
                                    std::vector<std::uint8_t>& output, std::size_t available) {
        alchemy::CallRegs r;
        const auto out = f.process.allocate_guest(available + 16), moved = f.process.allocate_guest(8);
        r.x = {op, 0, options, guest_buffer(f, key_bytes), key_bytes.size(), iv_bytes ? guest_buffer(f, *iv_bytes) : 0,
               guest_buffer(f, input), input.size()};
        r.push_stack(out); r.push_stack(static_cast<std::uint64_t>(available)); r.push_stack(moved);
        const auto status = static_cast<std::int32_t>(alchemy::engine_call_regs(f.engine, f.symbol(system, "_CCCrypt"), r).x0);
        output = guest_bytes(f, out, static_cast<std::size_t>(std::min<std::uint64_t>(f.engine.read64(moved), available)));
        return status;
    };
    {
        std::vector<std::uint8_t> out;
        require(crypt(0, 0, key, &iv, plain, out, 64) == 0 && out == hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"), "AES-128-CBC");
        require(crypt(0, 2, key, nullptr, std::vector<std::uint8_t>(plain.begin(), plain.begin() + 16), out, 16) == 0 &&
                out == hex("3ad77bb40d7a3660a89ecaf32466ef97"), "AES-128-ECB");
        require(crypt(0, 0, key, &iv, hex("0011"), out, 64) == -4303, "unpadded partial block");
        require(crypt(0, 1, key, &iv, plain, out, 16) == -4301, "kCCBufferTooSmall");
        require(crypt(0, 0, hex("0011"), &iv, plain, out, 64) == -4310, "kCCKeySizeError");
        std::vector<std::uint8_t> padded, back;
        const auto message = bytes("A license token, not a block multiple");
        require(crypt(0, 1, key, &iv, message, padded, 64) == 0 && padded.size() == 48, "PKCS7 encrypt");
        require(crypt(1, 1, key, &iv, padded, back, 64) == 0 && back == message, "PKCS7 decrypt");
        padded.back() ^= 1;
        require(crypt(1, 1, key, &iv, padded, back, 64) == -4304, "bad padding is kCCDecodeError");
        padded.back() ^= 1;
        // Streaming CCCryptor, one byte at a time, against the one-shot result.
        const auto ref = f.process.allocate_guest(8);
        alchemy::CallRegs create;
        create.x = {1, 0, 1, guest_buffer(f, key), key.size(), guest_buffer(f, iv), ref, 0};
        require(alchemy::engine_call_regs(f.engine, f.symbol(system, "_CCCryptorCreate"), create).x0 == 0, "CCCryptorCreate");
        const auto cryptor = f.engine.read64(ref);
        const auto out_buffer = f.process.allocate_guest(64), moved = f.process.allocate_guest(8);
        std::vector<std::uint8_t> streamed;
        for (const auto byte : padded) {
            const auto in = guest_buffer(f, {byte});
            require(f.sys("_CCCryptorUpdate", {cryptor, in, 1, out_buffer, 64, moved}) == 0, "CCCryptorUpdate");
            const auto produced = guest_bytes(f, out_buffer, static_cast<std::size_t>(f.engine.read64(moved)));
            streamed.insert(streamed.end(), produced.begin(), produced.end());
        }
        require(streamed.size() == 32, "decryptor keeps the last block back");
        require(f.sys("_CCCryptorFinal", {cryptor, out_buffer, 64, moved}) == 0, "CCCryptorFinal");
        const auto tail = guest_bytes(f, out_buffer, static_cast<std::size_t>(f.engine.read64(moved)));
        streamed.insert(streamed.end(), tail.begin(), tail.end());
        require(streamed == message, "streamed decryption");
        require(f.sys("_CCCryptorRelease", {cryptor}) == 0, "CCCryptorRelease");
        // RFC 6070 PBKDF2-HMAC-SHA1.
        const auto derive = [&f, system](std::string_view password, std::string_view salt, std::uint32_t rounds, std::size_t length) {
            alchemy::CallRegs r;
            const auto output = f.process.allocate_guest(length);
            r.x = {2, f.text(password), password.size(), f.text(salt), salt.size(), 1, rounds, output};
            r.push_stack(static_cast<std::uint64_t>(length));
            const auto status = alchemy::engine_call_regs(f.engine, f.symbol(system, "_CCKeyDerivationPBKDF"), r).x0;
            require(status == 0, "CCKeyDerivationPBKDF status");
            return guest_bytes(f, output, length);
        };
        require(derive("password", "salt", 1, 20) == hex("0c60c80f961f0e71f3a9b524af6012062fe037a6"), "PBKDF2 c=1");
        require(derive("password", "salt", 2, 20) == hex("ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"), "PBKDF2 c=2");
    }
    // CoreFoundation: external representations, file resources, @{}.
    {
        const auto string = f.cf.string("h\xc3\xa9llo");
        const auto utf8 = f.call(cf, "_CFStringCreateExternalRepresentation", {0, string, 0x08000100, 0});
        require(f.call(cf, "_CFDataGetLength", {utf8}) == 6, "UTF-8 external representation");
        const auto utf16 = f.call(cf, "_CFStringCreateExternalRepresentation", {0, string, 0x0100, 0});
        require(f.call(cf, "_CFDataGetLength", {utf16}) == 12, "UTF-16 external representation has a BOM");
        require(f.call(cf, "_CFStringCreateExternalRepresentation", {0, string, 0x0600, 0}) == 0, "lossy ASCII without a loss byte");
        const auto file = std::filesystem::temp_directory_path() / "alchemy_export_fill_resource.txt";
        { std::ofstream stream(file, std::ios::binary); stream << "resource bytes"; }
        const auto url = f.cf.file_url(alchemy::guest_path_string(file), false);
        const auto data = f.process.allocate_guest(8), properties = f.process.allocate_guest(8), error = f.process.allocate_guest(4);
        require(f.call(cf, "_CFURLCreateDataAndPropertiesFromResource", {0, url, data, properties, 0, error}) == 1, "CFURLCreateDataAndPropertiesFromResource");
        require(f.call(cf, "_CFDataGetLength", {f.engine.read64(data)}) == 14 && f.engine.read32(error) == 0, "resource data");
        const auto length = f.cf.dictionary_value(f.engine.read64(properties), f.cf.string("kCFURLFileLength"));
        require(length && f.cf.dictionary_value(f.engine.read64(properties), f.cf.string("kCFURLFileExists")), "resource properties");
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        require(f.call(cf, "_CFURLCreateDataAndPropertiesFromResource", {0, url, data, 0, 0, error}) == 0 &&
                static_cast<std::int32_t>(f.engine.read32(error)) == -12, "missing resource");
        const auto empty = f.symbol(cf, "___NSDictionary0__struct");
        require(f.call(cf, "_CFDictionaryGetCount", {empty}) == 0, "__NSDictionary0__struct");
    }
    // NSProcessInfo activities and -[NSBundle executableURL].
    {
        const auto info = f.objc.send(f.objc.find_class("NSProcessInfo"), "processInfo").x0;
        alchemy::CallRegs begin;
        begin.x[2] = 1ULL << 20; begin.x[3] = f.cf.string("rendering");  // NSActivityIdleSystemSleepDisabled
        const auto token = f.objc.send(info, "beginActivityWithOptions:reason:", begin).x0;
        require(token != 0, "beginActivityWithOptions:reason:");
        alchemy::CallRegs end;
        end.x[2] = token;
        static_cast<void>(f.objc.send(info, "endActivity:", end));
        const auto bundle = f.objc.send(f.objc.find_class("NSBundle"), "mainBundle").x0;
        const auto path = f.objc.send(bundle, "executablePath").x0, url = f.objc.send(bundle, "executableURL").x0;
        require(!path || (url && f.cf.url_path(url) == f.cf.utf8(path)), "executableURL matches executablePath");
    }
    // Foundation range and geometry functions.
    {
        const auto foundation = alchemy::foundation_library;
        alchemy::CallRegs ranges;
        ranges.x = {2, 5, 4, 10, 0, 0, 0, 0};
        const auto intersection = alchemy::engine_call_regs(f.engine, f.symbol(foundation, "_NSIntersectionRange"), ranges);
        require(!intersection.complete || (intersection.x0 == 4 && intersection.x1 == 3), "NSIntersectionRange");
        require(f.cf.utf8(f.call(foundation, "_NSStringFromRange", {3, 7})) == "{3, 7}", "NSStringFromRange");
        alchemy::CallRegs rect;
        rect.set_d(0, 1.5); rect.set_d(1, 2); rect.set_d(2, 30); rect.set_d(3, 40);
        require(f.cf.utf8(alchemy::engine_call_regs(f.engine, f.symbol(foundation, "_NSStringFromRect"), rect).x0) == "{{1.5, 2}, {30, 40}}", "NSStringFromRect");
        alchemy::CallRegs inside;
        inside.set_d(0, 10); inside.set_d(1, 41.9); inside.set_d(2, 1.5); inside.set_d(3, 2); inside.set_d(4, 30); inside.set_d(5, 40);
        require(alchemy::engine_call_regs(f.engine, f.symbol(foundation, "_NSPointInRect"), inside).x0 == 1, "NSPointInRect inside");
        inside.set_d(1, 42);
        require(alchemy::engine_call_regs(f.engine, f.symbol(foundation, "_NSPointInRect"), inside).x0 == 0, "NSPointInRect is half-open");
        require(!f.cf.utf8(f.call(foundation, "_NSUserName")).empty(), "NSUserName");
    }
    // IOKit user clients do not exist.
    {
        const auto service = f.call(iokit, "_IOServiceGetMatchingService", {0, f.call(iokit, "_IOServiceMatching", {f.text("IOPlatformExpertDevice")})});
        const auto connect = f.process.allocate_guest(4);
        require(service && f.call(iokit, "_IOServiceOpen", {service, 0, 0, connect}) == 0xe00002c7, "IOServiceOpen is unsupported");
        require(f.call(iokit, "_IOServiceOpen", {0, 0, 0, connect}) == 0xe00002c2, "IOServiceOpen of no service");
    }
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("fixture"); Fixture f;
        stage("guest services"); test_guest_services(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "guest services tests passed\n";
    return 0;
}
