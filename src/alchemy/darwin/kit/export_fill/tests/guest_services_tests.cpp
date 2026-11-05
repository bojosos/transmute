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
        const auto rounds = static_cast<std::uint32_t>(f.sys("_CCCalibratePBKDF", {2, 8, 16, 3, 32, 10}));
        require(rounds >= 1 && rounds != 0xffffffffu, "CCCalibratePBKDF");
    }
    // CCCryptorCreateWithMode: NIST SP 800-38A F.5.1 (CTR), F.3.13 (CFB128), F.4.1 (OFB), F.3.7 (CFB8), AES-128.
    {
        const auto with_mode = [&f, system](std::uint32_t op, std::uint32_t mode, std::uint32_t padding, const std::vector<std::uint8_t>& key_bytes,
                                            const std::vector<std::uint8_t>& iv_bytes, std::uint32_t options = 0) {
            alchemy::CallRegs r;
            const auto ref = f.process.allocate_guest(8);
            r.x = {op, mode, 0, padding, guest_buffer(f, iv_bytes), guest_buffer(f, key_bytes), key_bytes.size(), 0};
            r.push_stack(std::uint64_t{0});      // tweakLength
            r.push_stack(std::int32_t{0});       // numRounds
            r.push_stack(std::uint32_t{options});
            r.push_stack(ref);
            const auto status = static_cast<std::int32_t>(alchemy::engine_call_regs(f.engine, f.symbol(system, "_CCCryptorCreateWithMode"), r).x0);
            return std::pair{status, f.engine.read64(ref)};
        };
        // Update in the given chunk sizes, then Final; returns everything produced.
        const auto run = [&f](std::uint64_t cryptor, const std::vector<std::uint8_t>& input, std::initializer_list<std::size_t> chunks) {
            const auto out_buffer = f.process.allocate_guest(input.size() + 32), moved = f.process.allocate_guest(8);
            std::vector<std::uint8_t> output;
            std::size_t offset = 0;
            for (auto chunk : chunks) {
                chunk = std::min(chunk, input.size() - offset);
                const auto in = guest_buffer(f, std::vector<std::uint8_t>(input.begin() + static_cast<std::ptrdiff_t>(offset),
                                                                          input.begin() + static_cast<std::ptrdiff_t>(offset + chunk)));
                require(f.sys("_CCCryptorUpdate", {cryptor, in, chunk, out_buffer, input.size() + 32, moved}) == 0, "CCCryptorUpdate (mode)");
                const auto produced = guest_bytes(f, out_buffer, static_cast<std::size_t>(f.engine.read64(moved)));
                output.insert(output.end(), produced.begin(), produced.end());
                offset += chunk;
            }
            require(offset == input.size(), "test chunks cover the input");
            require(f.sys("_CCCryptorFinal", {cryptor, out_buffer, 32, moved}) == 0, "CCCryptorFinal (mode)");
            const auto tail = guest_bytes(f, out_buffer, static_cast<std::size_t>(f.engine.read64(moved)));
            output.insert(output.end(), tail.begin(), tail.end());
            require(f.sys("_CCCryptorRelease", {cryptor}) == 0, "CCCryptorRelease (mode)");
            return output;
        };
        const auto counter = hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        const struct { std::uint32_t mode; const std::vector<std::uint8_t>& iv; std::string_view expected; std::size_t length; } cases[] = {
            {4, counter, "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", 32},
            {3, iv, "3b3fd92eb72dad20333449f8e83cfb4ac8a64537a0b3a93fcde3cdad9f1ce58b", 32},
            {7, iv, "3b3fd92eb72dad20333449f8e83cfb4a7789508d16918f03f53c52dac54ed825", 32},
            {10, iv, "3b79424c9c0dd436bace9e0ed4586a4f32b9", 18}};
        for (const auto& item : cases) {
            const std::vector<std::uint8_t> input(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(item.length));
            auto [status, cryptor] = with_mode(0, item.mode, 0, key, item.iv, item.mode == 4 ? 2 : 0);
            require(status == 0 && cryptor, "CCCryptorCreateWithMode " + std::to_string(item.mode));
            const auto encrypted = run(cryptor, input, {3, 13, 1, 20});
            require(encrypted == hex(item.expected), "stream mode " + std::to_string(item.mode) + " ciphertext");
            std::tie(status, cryptor) = with_mode(1, item.mode, 0, key, item.iv);
            require(status == 0 && run(cryptor, encrypted, {7, 25, 4}) == input, "stream mode " + std::to_string(item.mode) + " decryption");
        }
        // CBC through CreateWithMode with PKCS#7 equals CCCrypt.
        auto [status, cryptor] = with_mode(0, 2, 1, key, iv);
        const auto message = bytes("A license token, not a block multiple");
        std::vector<std::uint8_t> expected;
        require(status == 0 && crypt(0, 1, key, &iv, message, expected, 64) == 0 && run(cryptor, message, {5, 40}) == expected,
                "CreateWithMode CBC");
        require(with_mode(0, 3, 1, key, iv).first == -4300, "padding with a stream mode is kCCParamError");
        require(with_mode(0, 8, 0, key, iv).first == -4305, "XTS is kCCUnimplemented");
    }
    // AES-GCM, the GCM specification's test cases 2-6 (AES-128).
    {
        const auto oneshot = [&f, system](bool encrypt, const std::vector<std::uint8_t>& key_bytes, const std::vector<std::uint8_t>& iv_bytes,
                                          const std::vector<std::uint8_t>& aad, const std::vector<std::uint8_t>& input,
                                          std::vector<std::uint8_t>& output, std::vector<std::uint8_t>& tag) {
            alchemy::CallRegs r;
            const auto out = f.process.allocate_guest(input.size() + 16);
            const auto tag_address = encrypt ? f.process.allocate_guest(16) : guest_buffer(f, tag);
            r.x = {0, guest_buffer(f, key_bytes), key_bytes.size(), guest_buffer(f, iv_bytes), iv_bytes.size(),
                   aad.empty() ? 0 : guest_buffer(f, aad), aad.size(), input.empty() ? 0 : guest_buffer(f, input)};
            r.push_stack(static_cast<std::uint64_t>(input.size()));
            r.push_stack(out);
            r.push_stack(tag_address);
            r.push_stack(std::uint64_t{16});
            const auto name = encrypt ? "_CCCryptorGCMOneshotEncrypt" : "_CCCryptorGCMOneshotDecrypt";
            const auto status = static_cast<std::int32_t>(alchemy::engine_call_regs(f.engine, f.symbol(system, name), r).x0);
            output = guest_bytes(f, out, input.size());
            if (encrypt) tag = guest_bytes(f, tag_address, 16);
            return status;
        };
        const auto gcm_key = hex("feffe9928665731c6d6a8f9467308308");
        const auto gcm_plain = hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525"
                                   "b16aedf5aa0de657ba637b391aafd255");
        const auto aad = hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        const std::vector<std::uint8_t> short_plain(gcm_plain.begin(), gcm_plain.begin() + 60);
        std::vector<std::uint8_t> out, tag, back;
        require(oneshot(true, std::vector<std::uint8_t>(16, 0), std::vector<std::uint8_t>(12, 0), {}, std::vector<std::uint8_t>(16, 0), out, tag) == 0 &&
                out == hex("0388dace60b6a392f328c2b971b2fe78") && tag == hex("ab6e47d42cec13bdf53a67b21257bddf"), "GCM test case 2");
        require(oneshot(true, gcm_key, hex("cafebabefacedbaddecaf888"), {}, gcm_plain, out, tag) == 0 &&
                out == hex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa05"
                           "1ba30b396a0aac973d58e091473f5985") && tag == hex("4d5c2af327cd64a62cf35abd2ba6fab4"), "GCM test case 3");
        require(oneshot(true, gcm_key, hex("cafebabefacedbaddecaf888"), aad, short_plain, out, tag) == 0 &&
                tag == hex("5bc94fbc3221a5db94fae95ae7121a47"), "GCM test case 4 (additional data)");
        require(oneshot(false, gcm_key, hex("cafebabefacedbaddecaf888"), aad, out, back, tag) == 0 && back == short_plain, "GCM decrypt");
        auto bad = tag;
        bad[3] ^= 0x10;
        require(oneshot(false, gcm_key, hex("cafebabefacedbaddecaf888"), aad, out, back, bad) == -4308, "GCM tag mismatch");
        require(oneshot(true, gcm_key, hex("cafebabefacedbad"), aad, short_plain, out, tag) == 0 &&
                tag == hex("3612d2e79e3b0785561be14aaca2fccb"), "GCM test case 5 (64-bit IV)");
        require(oneshot(true, gcm_key, hex("9313225df88406e555909c5aff5269aa6a7a9538534f7da1e4c303d2a318a728c3c0c95156809539fcf0e2429a6b5254"
                                           "16aedbf5a0de6a57a637b39b"), aad, short_plain, out, tag) == 0 &&
                tag == hex("619cc5aefffe0bfa462af43c1699d050"), "GCM test case 6 (60-byte IV)");
        // The older CCCryptorGCM (size_t *tagLength).
        alchemy::CallRegs r;
        const auto gcm_out = f.process.allocate_guest(64), tag_out = f.process.allocate_guest(16), tag_length = f.process.allocate_guest(8);
        f.engine.write64(tag_length, 16);
        r.x = {0, 0, guest_buffer(f, gcm_key), 16, guest_buffer(f, hex("cafebabefacedbaddecaf888")), 12, guest_buffer(f, aad), aad.size()};
        r.push_stack(guest_buffer(f, short_plain));
        r.push_stack(static_cast<std::uint64_t>(short_plain.size()));
        r.push_stack(gcm_out);
        r.push_stack(tag_out);
        r.push_stack(tag_length);
        require(alchemy::engine_call_regs(f.engine, f.symbol(system, "_CCCryptorGCM"), r).x0 == 0 &&
                guest_bytes(f, tag_out, 16) == hex("5bc94fbc3221a5db94fae95ae7121a47"), "CCCryptorGCM");
    }
    // RFC 3394 4.1 (128-bit KEK, 128-bit key), CCDigest.
    {
        const auto wrap = [&f, system](bool forward, const std::vector<std::uint8_t>& kek, const std::vector<std::uint8_t>& input,
                                       std::vector<std::uint8_t>& output) {
            alchemy::CallRegs r;
            const auto out = f.process.allocate_guest(64), length = f.process.allocate_guest(8);
            f.engine.write64(length, 64);
            const auto iv_address = f.engine.read64(f.symbol(system, "_CCrfc3394_iv"));
            r.x = {1, iv_address, f.engine.read64(f.symbol(system, "_CCrfc3394_ivLen")), guest_buffer(f, kek), kek.size(),
                   guest_buffer(f, input), input.size(), out};
            r.push_stack(length);
            const auto status = static_cast<std::int32_t>(alchemy::engine_call_regs(
                f.engine, f.symbol(system, forward ? "_CCSymmetricKeyWrap" : "_CCSymmetricKeyUnwrap"), r).x0);
            output = guest_bytes(f, out, static_cast<std::size_t>(std::min<std::uint64_t>(f.engine.read64(length), 64)));
            return status;
        };
        const auto kek = hex("000102030405060708090a0b0c0d0e0f"), raw = hex("00112233445566778899aabbccddeeff");
        std::vector<std::uint8_t> wrapped, unwrapped;
        require(wrap(true, kek, raw, wrapped) == 0 && wrapped == hex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"), "CCSymmetricKeyWrap");
        require(wrap(false, kek, wrapped, unwrapped) == 0 && unwrapped == raw, "CCSymmetricKeyUnwrap");
        wrapped[5] ^= 1;
        require(wrap(false, kek, wrapped, unwrapped) == -4304, "unwrap integrity check");
        require(f.sys("_CCSymmetricWrappedSize", {1, 16}) == 24 && f.sys("_CCSymmetricUnwrappedSize", {1, 24}) == 16, "wrap sizes");
        const auto digest_out = f.process.allocate_guest(64);
        require(f.sys("_CCDigest", {10, f.text("abc"), 3, digest_out}) == 0 &&
                guest_bytes(f, digest_out, 32) == hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "CCDigest SHA-256");
        require(f.sys("_CCDigestGetOutputSize", {12}) == 64 && f.sys("_CCDigestGetOutputSize", {99}) == 0, "CCDigestGetOutputSize");
        require(static_cast<std::int32_t>(f.sys("_CCDigest", {1, f.text("abc"), 3, digest_out})) == -4305, "MD2 is kCCUnimplemented");
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
