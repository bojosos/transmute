// Host tests of crypto_core (the CryptoKit overlay's cryptography).
// Known-answer vectors: FIPS 180-4 examples (SHA-256/384/512), RFC 4231
// (HMAC-SHA512), RFC 5869 appendix A.1-A.3 (HKDF-SHA256), RFC 8439 sections
// 2.3.2, 2.5.2 and 2.8.2 (ChaCha20, Poly1305, AEAD). The P-521 product
// vectors were computed with an independent affine big-integer reference.
// Plain main(): prints each failed check and returns nonzero on failure.

#include "alchemy/darwin/security/crypto_core.hpp"

#include <cstdio>
#include <random>
#include <string>
#include <string_view>

using namespace alchemy::gui::completion_kit::crypto;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

Bytes hex(std::string_view text) {
    Bytes out;
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(nibble(text[i]) << 4 | nibble(text[i + 1])));
    return out;
}

Bytes text(std::string_view s) { return Bytes(s.begin(), s.end()); }

Bytes range(int from, int to) {
    Bytes out;
    for (int v = from; v < to; ++v) out.push_back(static_cast<std::uint8_t>(v));
    return out;
}

template <std::size_t N>
std::array<std::uint8_t, N> fixed(const Bytes& bytes) {
    std::array<std::uint8_t, N> out{};
    if (bytes.size() == N) std::copy(bytes.begin(), bytes.end(), out.begin());
    else check(false, "fixed-size vector length");
    return out;
}

template <typename A, typename B>
bool same(const A& a, const B& b) { return Bytes(a.begin(), a.end()) == Bytes(b.begin(), b.end()); }

void test_sha2() {
    const auto two_block_256 = text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    const auto two_block_512 = text("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu");
    check(same(sha256({}), hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")), "SHA-256 empty");
    check(same(sha256(text("abc")), hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")), "SHA-256 abc");
    check(same(sha256(two_block_256), hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")), "SHA-256 two blocks");
    check(same(hash(Hash::sha384, text("abc")), hex("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7")), "SHA-384 abc");
    check(same(hash(Hash::sha384, two_block_512), hex("09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039")), "SHA-384 two blocks");
    check(same(sha512({}), hex("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e")), "SHA-512 empty");
    check(same(sha512(text("abc")), hex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")), "SHA-512 abc");
    check(same(sha512(two_block_512), hex("8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909")), "SHA-512 two blocks");
    const Bytes million(1000000, 'a');
    check(same(sha256(million), hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")), "SHA-256 million a");
    check(same(sha512(million), hex("e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b")), "SHA-512 million a");
    // Incremental updates of every split agree with the one-shot digest.
    const auto message = range(0, 251);
    for (const auto kind : {Hash::sha256, Hash::sha384, Hash::sha512}) {
        const auto whole = hash(kind, message);
        bool all = true;
        for (std::size_t split = 0; split <= message.size(); ++split) {
            Hasher hasher(kind);
            hasher.update(ByteView(message).first(split));
            hasher.update(ByteView(message).subspan(split));
            all = all && hasher.finish() == whole;
        }
        check(all, "incremental hashing, kind " + std::to_string(static_cast<int>(kind)));
    }
}

void test_hmac_hkdf() {
    // RFC 4231 test cases 1, 2 and 6 (HMAC-SHA-512).
    check(hmac(Hash::sha512, Bytes(20, 0x0b), text("Hi There")) ==
              hex("87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"),
          "HMAC-SHA512 RFC 4231 case 1");
    check(hmac(Hash::sha512, text("Jefe"), text("what do ya want for nothing?")) ==
              hex("164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"),
          "HMAC-SHA512 RFC 4231 case 2");
    check(hmac(Hash::sha512, Bytes(131, 0xaa), text("Test Using Larger Than Block-Size Key - Hash Key First")) ==
              hex("80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598"),
          "HMAC-SHA512 RFC 4231 case 6");
    // RFC 5869 A.1-A.3 (HKDF-SHA256).
    check(hkdf_extract(Hash::sha256, range(0, 13), Bytes(22, 0x0b)) == hex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"), "HKDF A.1 PRK");
    check(hkdf(Hash::sha256, Bytes(22, 0x0b), range(0, 13), range(0xf0, 0xfa), 42) ==
              hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"), "HKDF A.1 OKM");
    check(hkdf_extract(Hash::sha256, range(0x60, 0xb0), range(0, 0x50)) == hex("06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244"), "HKDF A.2 PRK");
    check(hkdf(Hash::sha256, range(0, 0x50), range(0x60, 0xb0), range(0xb0, 0x100), 82) ==
              hex("b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"),
          "HKDF A.2 OKM");
    check(hkdf_extract(Hash::sha256, {}, Bytes(22, 0x0b)) == hex("19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"), "HKDF A.3 PRK");
    check(hkdf(Hash::sha256, Bytes(22, 0x0b), {}, {}, 42) ==
              hex("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"), "HKDF A.3 OKM");
    // HKDF-SHA512 against the reference (A.1 inputs, two output blocks).
    check(hkdf(Hash::sha512, Bytes(22, 0x0b), range(0, 13), range(0xf0, 0xfa), 100) ==
              hex("832390086cda71fb47625bb5ceb168e4c8e26a1a16ed34d9fc7fe92c1481579338da362cb8d9f925d7cbcce0dff7098769cf15959867d571c1715450cb530137be3fb62f3cf32b84feba8f1eb1b563e20d9749b8640b8264c4b69b14ad5199115e1d609c"),
          "HKDF-SHA512 two blocks");
    check(hkdf(Hash::sha512, Bytes(22, 0x0b), {}, {}, 255 * 64).has_value(), "HKDF-SHA512 255 blocks");
    check(!hkdf(Hash::sha512, Bytes(22, 0x0b), {}, {}, 255 * 64 + 1).has_value(), "HKDF-SHA512 length limit");
    check(hkdf(Hash::sha512, Bytes(22, 0x0b), {}, {}, 0) == Bytes{}, "HKDF zero length");
}

void test_chacha_poly() {
    // RFC 8439 2.3.2.
    check(same(chacha20_block(fixed<32>(range(0, 32)), 1, fixed<12>(hex("000000090000004a00000000"))),
               hex("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e")),
          "ChaCha20 block RFC 8439 2.3.2");
    // RFC 8439 2.5.2.
    check(same(poly1305(fixed<32>(hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b")), text("Cryptographic Forum Research Group")),
               hex("a8061dc1305136c6c22b8baf0c0127a9")),
          "Poly1305 RFC 8439 2.5.2");
    // RFC 8439 2.8.2.
    const auto key = fixed<32>(range(0x80, 0xa0));
    const auto nonce = fixed<12>(hex("070000004041424344454647"));
    const auto aad = hex("50515253c0c1c2c3c4c5c6c7");
    const auto plaintext = text("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
    const auto expected = hex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
    const auto sealed = aead_seal(key, nonce, aad, plaintext);
    check(sealed.ciphertext == expected, "AEAD RFC 8439 2.8.2 ciphertext");
    check(same(sealed.tag, hex("1ae10b594f09e26a7e902ecbd0600691")), "AEAD RFC 8439 2.8.2 tag");
    check(aead_open(key, nonce, aad, sealed.ciphertext, sealed.tag) == plaintext, "AEAD open");
    auto tampered = sealed.ciphertext;
    tampered[17] ^= 1;
    check(!aead_open(key, nonce, aad, tampered, sealed.tag), "AEAD rejects a modified ciphertext");
    auto bad_tag = sealed.tag;
    bad_tag[15] ^= 0x80;
    check(!aead_open(key, nonce, aad, sealed.ciphertext, bad_tag), "AEAD rejects a modified tag");
    check(!aead_open(key, nonce, {}, sealed.ciphertext, sealed.tag), "AEAD rejects different associated data");
    // Round trips across block boundaries, no associated data (CryptoKit's seal).
    for (std::size_t size : {0u, 1u, 15u, 16u, 17u, 63u, 64u, 65u, 1000u}) {
        const Bytes message(size, static_cast<std::uint8_t>(size));
        const auto box = aead_seal(key, nonce, {}, message);
        check(box.ciphertext.size() == size && aead_open(key, nonce, {}, box.ciphertext, box.tag) == message,
              "AEAD round trip " + std::to_string(size));
    }
}

P521Scalar scalar(std::string_view h) { return fixed<66>(hex(h)); }
P521Point point(std::string_view h) { return fixed<132>(hex(h)); }

void test_p521() {
    const auto& g = p521_generator;
    check(p521_valid_public(g), "G is on the curve");
    P521Scalar one{};
    one[65] = 1;
    check(p521_multiply(one, g) == g, "1 * G == G");
    check(!p521_multiply(p521_order, g).has_value(), "n * G is the point at infinity");
    check(!p521_multiply(P521Scalar{}, g).has_value(), "0 * G is the point at infinity");
    auto n_minus_1 = p521_order;
    n_minus_1[65] -= 1;
    const auto minus_g = p521_multiply(n_minus_1, g);
    check(minus_g && same(std::span(*minus_g).first(66), std::span(g).first(66)) &&
              same(std::span(*minus_g).subspan(66), hex("00e7c6d6958765c43ffba375a04bd382e426670abbb6a864bb97e85042e8d8c199d368118d66a10bd9bf3aaf46fec052f89ecac38f795d8d3dbf77416b89602e99af")),
          "(n - 1) * G == -G");
    P521Scalar two{};
    two[65] = 2;
    check(p521_multiply(two, g) == point("00433c219024277e7e682fcb288148c282747403279b1ccc06352c6e5505d769be97b3b204da6ef55507aa104a3a35c5af41cf2fa364d60fd967f43e3933ba6d783d00f4bb8cc7f86db26700a7f3eceeeed3f0b5c6b5107c4da97740ab21a29906c42dbbb3e377de9f251f6b93937fa99a3248f4eafcbe95edc0f4f71be356d661f41b02"),
          "2 * G");
    // Private key range 1 .. n - 1.
    check(!p521_valid_private(P521Scalar{}), "private key 0 is invalid");
    check(!p521_valid_private(p521_order), "private key n is invalid");
    check(p521_valid_private(n_minus_1) && p521_valid_private(one), "private keys 1 and n - 1 are valid");
    check(!p521_public_key(p521_order).has_value(), "no public key for n");
    // Fixed key pairs and their shared secret (reference values).
    const auto a = scalar("0000cc9443cd14e35cd1452c027649c28b72031a932d67741737cc12c58c3cae903c0c9eb276fff2a8283a288be4b8d9e4708077fa45b5b1a9ad8a7e4a34d5505342");
    const auto a_public = point("00cc1aa053c74395625d4b83ce7625ef87442bd04bea252bf23bc5f43b2aefcdc79b3201fc78fa1ff2a454ce10a45adb530191a611ef411ff96810dd8ffe60b0c42e001a10494c992a5238ef076419259da8824ffdb43e2e438195de0adac390a9c0984390b0967bbd04b6917dcb81dd54334248187eb0df355b29f7f3aaf44c3a02135d");
    const auto b = scalar("006d6c6a7b2615bb3f732989965fabbb0b759b1567c926bb5dd0e4acf326eee8d4f84eb6f5700921765afa3efa94a9290da8df475e7dac38302ca28fe8deabdda7ab");
    const auto b_public = point("013cad8c96dda2953b7977f8bf9a0ccdc60a9ba50fbe839f52d276cb926291b90c41a04382dea29f3445f702c6a481b69ce376710d7419116f7edb71604e8f86ee1a0050fd66a712df8c8396d5a32305d246604479cdf2c06d45148c4ce289fb1fce973113d6340c799beb9cc3d1a8fa7dbb0be3fa50d755b20b7d3374f74ea4038fc3b2");
    const auto secret = scalar("019d992bc59c4238afecb2ce47f9dcaf0a470ecf7dabd2794d5dc86c6e9184dee43d79c339fd7720dff812cd3d773b6d0211a9aa4930eb300585a054a39442145b66");
    check(p521_public_key(a) == a_public, "public key of a");
    check(p521_public_key(b) == b_public, "public key of b");
    check(p521_shared_secret(a, b_public) == secret, "ECDH(a, bG)");
    check(p521_shared_secret(b, a_public) == secret, "ECDH(b, aG)");
    // Invalid points are rejected.
    auto off_curve = a_public;
    off_curve[131] ^= 1;
    check(!p521_valid_public(off_curve) && !p521_shared_secret(b, off_curve), "a point off the curve is rejected");
    P521Point too_large{};
    too_large.fill(0xff);
    check(!p521_valid_public(too_large), "coordinates >= p are rejected");
    check(!p521_valid_public(P521Point{}), "(0, 0) is rejected");
    // Generated keys: valid, compact when asked, and agreeing.
    std::mt19937_64 engine(521);   // deterministic test randomness only
    const auto random = [&engine](std::span<std::uint8_t> bytes) {
        for (auto& byte : bytes) byte = static_cast<std::uint8_t>(engine());
    };
    bool compact_all = true, valid_all = true, agree_all = true, non_compact_seen = false;
    for (int round = 0; round < 6; ++round) {
        const auto first = p521_generate(random, true);
        const auto second = p521_generate(random, round % 2 == 0);
        valid_all = valid_all && p521_valid_private(first.private_key) && p521_valid_public(first.public_key) &&
                    p521_valid_public(second.public_key) && p521_public_key(first.private_key) == first.public_key;
        compact_all = compact_all && p521_compact(first.public_key);
        non_compact_seen = non_compact_seen || !p521_compact(second.public_key);
        agree_all = agree_all && p521_shared_secret(first.private_key, second.public_key) == p521_shared_secret(second.private_key, first.public_key);
    }
    check(valid_all, "generated keys validate");
    check(compact_all, "compact generation yields y <= p - y");
    check(agree_all, "ECDH of generated keys agrees");
    static_cast<void>(non_compact_seen);   // non-compact keys are possible, not guaranteed, without the flag
    // The negated point of a compact key is not compact (unless y == 0, impossible here).
    check(p521_compact(minus_g.value_or(P521Point{})) != p521_compact(g), "exactly one of G and -G is compact");
}

} // namespace

int main() {
    test_sha2();
    test_hmac_hkdf();
    test_chacha_poly();
    test_p521();
    if (failures) {
        std::printf("crypto_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("crypto_tests: all checks passed\n");
    return 0;
}
