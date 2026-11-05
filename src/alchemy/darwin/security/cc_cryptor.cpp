#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/host/crypto.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// CommonCrypto symmetric ciphers and PBKDF2 (libcommonCrypto, re-exported by
// libSystem): CCCryptorCreate / CreateWithMode / Update / Final / Release /
// GetOutputLength / Reset, the one-shot CCCrypt, AES-GCM one-shots
// (CCCryptorGCMOneshotEncrypt / Decrypt and the older CCCryptorGCM), RFC 3394
// AES key wrap (CCSymmetricKeyWrap / Unwrap and the size helpers),
// CCKeyDerivationPBKDF, CCCalibratePBKDF, and CCDigest /
// CCDigestGetOutputSize. HMAC, the CC_* digests and random bytes are the
// network/security provider's (src/alchemy/security).
//
// Block ciphers run on the host's (host::crypto::BlockCipher: AES, DES, 3DES)
// in ECB mode, one block at a time; the modes (CBC, CFB, CTR with a
// big-endian counter, OFB, CFB8, GCM's counter and GHASH) and PKCS#7 padding
// are done here so streaming Update calls behave as CommonCrypto's (a
// decryptor with padding keeps the last block back until Final). Other
// algorithms (CAST, RC4, RC2, Blowfish), other modes (F8, LRW, XTS, CCM,
// streaming GCM), MD2 / MD4 / RIPEMD digests and PBKDF2 with HMAC-SHA224 where
// the host lacks it report kCCUnimplemented.
namespace alchemy::export_fill {
namespace {

constexpr std::int32_t cc_success = 0, cc_param_error = -4300, cc_buffer_too_small = -4301, cc_alignment_error = -4303,
    cc_decode_error = -4304, cc_unimplemented = -4305, cc_unspecified_error = -4308, cc_key_size_error = -4310;
// CCMode.
constexpr std::uint32_t mode_ecb = 1, mode_cbc = 2, mode_cfb = 3, mode_ctr = 4, mode_ofb = 7, mode_cfb8 = 10;
constexpr std::uint32_t op_encrypt = 0, op_decrypt = 1;
constexpr std::uint32_t option_pkcs7 = 1, option_ecb = 2;

std::uint64_t status(std::int32_t value) { return static_cast<std::uint32_t>(value); }

struct Cryptor {
    std::uint32_t operation{}, options{};
    std::uint32_t stream_mode{};   // CFB, CTR, OFB or CFB8: byte streams without padding (0: ECB / CBC)
    std::size_t block{};
    std::vector<std::uint8_t> iv, initial_iv, pending;
    std::vector<std::uint8_t> keystream, feedback;   // stream modes: the current keystream block, CFB's next register
    std::size_t used{};                              // bytes of `keystream` consumed (block: none left)
    host::crypto::BlockCipher cipher;

    // One block encrypted (the stream modes and GCM only ever encrypt).
    void encrypt_block(const std::uint8_t* input, std::uint8_t* output) const {
        std::array<std::uint8_t, 16> in{}, out{};
        std::memcpy(in.data(), input, block);
        if (!cipher.encrypt(std::as_bytes(std::span(in.data(), block)), std::as_writable_bytes(std::span(out.data(), block))))
            throw std::runtime_error("Alchemy CommonCrypto: CNG block operation failed");
        std::memcpy(output, out.data(), block);
    }
    void restart_stream() {
        keystream.assign(block, 0);
        feedback.assign(block, 0);
        used = block;
    }
    // Stream modes: `input` through the keystream (CTR, OFB, CFB, CFB8).
    std::vector<std::uint8_t> stream(std::span<const std::uint8_t> input) {
        std::vector<std::uint8_t> output(input.size());
        for (std::size_t index = 0; index < input.size(); ++index) {
            const auto in = input[index];
            if (stream_mode == mode_cfb8) {
                encrypt_block(iv.data(), keystream.data());
                const auto out = static_cast<std::uint8_t>(in ^ keystream[0]);
                const auto cipher_byte = operation == op_encrypt ? out : in;
                std::memmove(iv.data(), iv.data() + 1, block - 1);
                iv[block - 1] = cipher_byte;
                output[index] = out;
                continue;
            }
            if (used == block) {
                if (stream_mode == mode_ctr) {
                    encrypt_block(iv.data(), keystream.data());
                    for (std::size_t at = block; at-- > 0;) if (++iv[at]) break;   // the big-endian counter
                } else if (stream_mode == mode_ofb) {
                    encrypt_block(iv.data(), keystream.data());
                    iv = keystream;
                } else {   // CFB: the register is the previous ciphertext block
                    encrypt_block(iv.data(), keystream.data());
                }
                used = 0;
            }
            const auto out = static_cast<std::uint8_t>(in ^ keystream[used]);
            if (stream_mode == mode_cfb) {
                feedback[used] = operation == op_encrypt ? out : in;
                if (used + 1 == block) iv = feedback;
            }
            ++used;
            output[index] = out;
        }
        return output;
    }

    void crypt_block(const std::uint8_t* input, std::uint8_t* output) const {
        std::array<std::uint8_t, 16> in{}, out{};
        std::memcpy(in.data(), input, block);
        const auto source = std::as_bytes(std::span(in.data(), block));
        const auto target = std::as_writable_bytes(std::span(out.data(), block));
        const auto result = operation == op_encrypt ? cipher.encrypt(source, target) : cipher.decrypt(source, target);
        if (!result) {
            if (result.error() == host::Error::unsupported) throw std::runtime_error("Alchemy CommonCrypto ciphers require the host's block ciphers");
            throw std::runtime_error("Alchemy CommonCrypto: CNG block operation failed");
        }
        std::memcpy(output, out.data(), block);
    }
    // One block through the mode (ECB or CBC).
    void process_block(const std::uint8_t* input, std::uint8_t* output) {
        std::array<std::uint8_t, 16> work{};
        if (options & option_ecb) { crypt_block(input, output); return; }
        if (operation == op_encrypt) {
            for (std::size_t index = 0; index < block; ++index) work[index] = static_cast<std::uint8_t>(input[index] ^ iv[index]);
            crypt_block(work.data(), output);
            std::memcpy(iv.data(), output, block);
        } else {
            std::array<std::uint8_t, 16> saved{};
            std::memcpy(saved.data(), input, block);
            crypt_block(input, work.data());
            for (std::size_t index = 0; index < block; ++index) output[index] = static_cast<std::uint8_t>(work[index] ^ iv[index]);
            std::memcpy(iv.data(), saved.data(), block);
        }
    }
    bool holds_back() const { return operation == op_decrypt && (options & option_pkcs7); }
    // Bytes Update would produce for `length` more input.
    std::size_t update_length(std::size_t length) const {
        if (stream_mode) return length;
        auto total = pending.size() + length;
        auto blocks = total / block;
        if (holds_back() && blocks && total % block == 0) --blocks;
        return blocks * block;
    }
    std::size_t final_length() const {
        if (stream_mode) return 0;
        if (operation == op_encrypt) return (options & option_pkcs7) ? (pending.size() / block + 1) * block : pending.size();
        return pending.size();
    }
    std::vector<std::uint8_t> update(std::span<const std::uint8_t> input) {
        if (stream_mode) return stream(input);
        pending.insert(pending.end(), input.begin(), input.end());
        const auto produce = update_length(0);
        std::vector<std::uint8_t> output(produce);
        for (std::size_t offset = 0; offset < produce; offset += block) process_block(pending.data() + offset, output.data() + offset);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(produce));
        return output;
    }
    std::int32_t finish(std::vector<std::uint8_t>& output) {
        output.clear();
        if (stream_mode) return cc_success;
        if (operation == op_encrypt) {
            if (!(options & option_pkcs7)) {
                if (!pending.empty()) return cc_alignment_error;
                return cc_success;
            }
            const auto pad = static_cast<std::uint8_t>(block - pending.size() % block);
            pending.insert(pending.end(), pad, pad);
            output.resize(pending.size());
            for (std::size_t offset = 0; offset < pending.size(); offset += block) process_block(pending.data() + offset, output.data() + offset);
            pending.clear();
            return cc_success;
        }
        if (pending.size() % block) return cc_alignment_error;
        if (!(options & option_pkcs7)) return cc_success;
        if (pending.empty()) return cc_decode_error;
        output.resize(pending.size());
        for (std::size_t offset = 0; offset < pending.size(); offset += block) process_block(pending.data() + offset, output.data() + offset);
        pending.clear();
        const auto pad = output.back();
        if (pad == 0 || pad > block || pad > output.size()) { output.clear(); return cc_decode_error; }
        for (std::size_t index = output.size() - pad; index < output.size(); ++index)
            if (output[index] != pad) { output.clear(); return cc_decode_error; }
        output.resize(output.size() - pad);
        return cc_success;
    }
};

// A cryptor for (op, algorithm, options, key, iv), or a CommonCrypto status.
std::int32_t make_cryptor(std::uint32_t operation, std::uint32_t algorithm, std::uint32_t options,
                          std::span<const std::uint8_t> key, std::span<const std::uint8_t> iv, std::unique_ptr<Cryptor>& out,
                          std::uint32_t stream_mode = 0) {
    if (operation != op_encrypt && operation != op_decrypt) return cc_param_error;
    if (options & ~(option_pkcs7 | option_ecb)) return cc_param_error;
    auto cryptor = std::make_unique<Cryptor>();
    cryptor->operation = operation;
    cryptor->options = options;
    cryptor->stream_mode = stream_mode;
    host::crypto::BlockAlgorithm kind{};
    switch (algorithm) {
    case 0: // kCCAlgorithmAES
        if (key.size() != 16 && key.size() != 24 && key.size() != 32) return cc_key_size_error;
        kind = host::crypto::BlockAlgorithm::aes; cryptor->block = 16; break;
    case 1: // kCCAlgorithmDES
        if (key.size() != 8) return cc_key_size_error;
        kind = host::crypto::BlockAlgorithm::des; cryptor->block = 8; break;
    case 2: // kCCAlgorithm3DES
        if (key.size() != 24) return cc_key_size_error;
        kind = host::crypto::BlockAlgorithm::triple_des; cryptor->block = 8; break;
    default: return cc_unimplemented; // CAST, RC4, RC2, Blowfish
    }
    auto cipher = host::crypto::BlockCipher::create(kind, std::as_bytes(key));
    if (!cipher) return cipher.error() == host::Error::invalid_argument ? cc_key_size_error : cc_unimplemented;
    cryptor->cipher = std::move(*cipher);
    cryptor->iv.assign(cryptor->block, 0);
    if (!iv.empty()) std::memcpy(cryptor->iv.data(), iv.data(), cryptor->block);
    cryptor->initial_iv = cryptor->iv;
    cryptor->restart_stream();
    out = std::move(cryptor);
    return cc_success;
}

// ---- AES-GCM (NIST SP 800-38D) -----------------------------------------------------------------------------

using Block = std::array<std::uint8_t, 16>;

// X * Y in GF(2^128) with GCM's bit order.
Block gf_multiply(const Block& x, const Block& y) {
    Block z{}, v = y;
    for (int bit = 0; bit < 128; ++bit) {
        if (x[bit / 8] & (0x80 >> (bit % 8)))
            for (int index = 0; index < 16; ++index) z[index] ^= v[index];
        const bool low = v[15] & 1;
        for (int index = 15; index > 0; --index) v[index] = static_cast<std::uint8_t>((v[index] >> 1) | (v[index - 1] << 7));
        v[0] >>= 1;
        if (low) v[0] ^= 0xe1;
    }
    return z;
}
// GHASH_H over `data` padded to blocks, continuing from `state`.
void ghash(Block& state, const Block& h, std::span<const std::uint8_t> data) {
    for (std::size_t offset = 0; offset < data.size(); offset += 16) {
        for (std::size_t index = 0; index < 16 && offset + index < data.size(); ++index) state[index] ^= data[offset + index];
        state = gf_multiply(state, h);
    }
}
void put_bit_length(Block& block, std::size_t at, std::uint64_t bytes) {
    const auto bits = bytes * 8;
    for (int index = 0; index < 8; ++index) block[at + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(bits >> (56 - 8 * index));
}

// AES-GCM over `input` (encrypting or decrypting); returns the full 16-byte tag.
std::int32_t gcm(bool encrypt, std::span<const std::uint8_t> key, std::span<const std::uint8_t> iv, std::span<const std::uint8_t> aad,
                 std::span<const std::uint8_t> input, std::vector<std::uint8_t>& output, Block& tag) {
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return cc_key_size_error;
    if (iv.empty()) return cc_param_error;
    auto cipher = host::crypto::BlockCipher::create(host::crypto::BlockAlgorithm::aes, std::as_bytes(key));
    if (!cipher) return cc_unimplemented;
    const auto encrypt_block = [&](const Block& in) {
        Block out{};
        if (!cipher->encrypt(std::as_bytes(std::span(in)), std::as_writable_bytes(std::span(out))))
            throw std::runtime_error("Alchemy CommonCrypto: CNG block operation failed");
        return out;
    };
    const auto h = encrypt_block(Block{});
    Block j0{};
    if (iv.size() == 12) {
        std::memcpy(j0.data(), iv.data(), 12);
        j0[15] = 1;
    } else {
        ghash(j0, h, iv);
        Block lengths{};
        put_bit_length(lengths, 8, iv.size());
        ghash(j0, h, lengths);
    }
    // The counter blocks from inc32(J0).
    output.resize(input.size());
    Block counter = j0;
    for (std::size_t offset = 0; offset < input.size(); offset += 16) {
        for (int index = 15; index >= 12; --index) if (++counter[static_cast<std::size_t>(index)]) break;
        const auto keystream = encrypt_block(counter);
        for (std::size_t index = 0; index < 16 && offset + index < input.size(); ++index)
            output[offset + index] = static_cast<std::uint8_t>(input[offset + index] ^ keystream[index]);
    }
    const auto& ciphertext = encrypt ? output : std::vector<std::uint8_t>(input.begin(), input.end());
    Block state{};
    ghash(state, h, aad);
    ghash(state, h, ciphertext);
    Block lengths{};
    put_bit_length(lengths, 0, aad.size());
    put_bit_length(lengths, 8, ciphertext.size());
    ghash(state, h, lengths);
    const auto mask = encrypt_block(j0);
    for (int index = 0; index < 16; ++index) tag[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(state[static_cast<std::size_t>(index)] ^ mask[static_cast<std::size_t>(index)]);
    return cc_success;
}

// ---- AES key wrap (RFC 3394) ---------------------------------------------------------------------------------

std::int32_t aes_key_wrap(bool wrap, std::span<const std::uint8_t> kek, std::span<const std::uint8_t> iv,
                          std::span<const std::uint8_t> input, std::vector<std::uint8_t>& output) {
    if (kek.size() != 16 && kek.size() != 24 && kek.size() != 32) return cc_param_error;
    if (iv.size() != 8) return cc_param_error;
    const auto blocks = wrap ? input.size() / 8 : input.size() / 8 - 1;
    if (input.size() % 8 || input.size() < (wrap ? 16 : 24)) return cc_param_error;
    auto cipher = host::crypto::BlockCipher::create(host::crypto::BlockAlgorithm::aes, std::as_bytes(kek));
    if (!cipher) return cc_unimplemented;
    std::array<std::uint8_t, 8> a{};
    std::vector<std::array<std::uint8_t, 8>> r(blocks);
    if (wrap) {
        std::memcpy(a.data(), iv.data(), 8);
        for (std::size_t index = 0; index < blocks; ++index) std::memcpy(r[index].data(), input.data() + 8 * index, 8);
    } else {
        std::memcpy(a.data(), input.data(), 8);
        for (std::size_t index = 0; index < blocks; ++index) std::memcpy(r[index].data(), input.data() + 8 * (index + 1), 8);
    }
    Block in{}, out{};
    const auto run = [&](bool forward) {
        const auto source = std::as_bytes(std::span(in));
        const auto target = std::as_writable_bytes(std::span(out));
        if (!(forward ? cipher->encrypt(source, target) : cipher->decrypt(source, target)))
            throw std::runtime_error("Alchemy CommonCrypto: CNG block operation failed");
    };
    for (int round = 0; round < 6; ++round) {
        for (std::size_t step = 0; step < blocks; ++step) {
            const auto j = wrap ? round : 5 - round;
            const auto i = wrap ? step : blocks - 1 - step;
            const auto t = static_cast<std::uint64_t>(blocks) * static_cast<std::uint64_t>(j) + i + 1;
            if (wrap) {
                std::memcpy(in.data(), a.data(), 8);
                std::memcpy(in.data() + 8, r[i].data(), 8);
                run(true);
                std::memcpy(a.data(), out.data(), 8);
                for (int index = 0; index < 8; ++index) a[static_cast<std::size_t>(index)] ^= static_cast<std::uint8_t>(t >> (56 - 8 * index));
                std::memcpy(r[i].data(), out.data() + 8, 8);
            } else {
                for (int index = 0; index < 8; ++index) a[static_cast<std::size_t>(index)] ^= static_cast<std::uint8_t>(t >> (56 - 8 * index));
                std::memcpy(in.data(), a.data(), 8);
                std::memcpy(in.data() + 8, r[i].data(), 8);
                run(false);
                std::memcpy(a.data(), out.data(), 8);
                std::memcpy(r[i].data(), out.data() + 8, 8);
            }
        }
    }
    output.clear();
    if (wrap) {
        output.insert(output.end(), a.begin(), a.end());
    } else if (!std::equal(a.begin(), a.end(), iv.begin())) {
        return cc_decode_error;   // the integrity check failed
    }
    for (const auto& item : r) output.insert(output.end(), item.begin(), item.end());
    return cc_success;
}

// ---- digests -------------------------------------------------------------------------------------------------

// CCDigestAlgorithm (CommonDigestSPI.h): SHA-1 8, SHA-224 9, SHA-256 10, SHA-384 11, SHA-512 12, MD5 3.
std::optional<host::crypto::Digest> digest_of(std::uint32_t algorithm, std::size_t& size) {
    switch (algorithm) {
    case 3: size = 16; return host::crypto::Digest::md5;
    case 8: size = 20; return host::crypto::Digest::sha1;
    case 9: size = 28; return host::crypto::Digest::sha224;
    case 10: size = 32; return host::crypto::Digest::sha256;
    case 11: size = 48; return host::crypto::Digest::sha384;
    case 12: size = 64; return host::crypto::Digest::sha512;
    default: size = 0; return std::nullopt;
    }
}

std::vector<std::uint8_t> read(GuestMemory& memory, std::uint64_t address, std::uint64_t length) {
    if (length > (1ULL << 31)) throw std::runtime_error("Alchemy CommonCrypto: buffer larger than 2 GiB");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (length) {
        if (!address) throw std::runtime_error("Alchemy CommonCrypto: null input buffer");
        memory.read_bytes(address, bytes);
    }
    return bytes;
}

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Cryptor>> cryptors;
};

} // namespace

std::int32_t test_pbkdf2(std::uint32_t prf, std::span<const std::uint8_t> password, std::span<const std::uint8_t> salt,
                         std::uint32_t rounds, std::span<std::uint8_t> output) {
    if (!rounds || output.empty()) return cc_param_error;
    host::crypto::Digest digest{};
    switch (prf) {
    case 1: digest = host::crypto::Digest::sha1; break;
    case 2: digest = host::crypto::Digest::sha224; break;
    case 3: digest = host::crypto::Digest::sha256; break;
    case 4: digest = host::crypto::Digest::sha384; break;
    case 5: digest = host::crypto::Digest::sha512; break;
    default: return cc_param_error;
    }
    const auto result = host::crypto::pbkdf2(digest, std::as_bytes(password), std::as_bytes(salt), rounds, std::as_writable_bytes(output));
    if (result) return cc_success;
    return result.error() == host::Error::unsupported ? cc_unimplemented : cc_param_error;
}

void install_cc_cryptor(Provider& p) {
    auto& process = p.process();
    auto registry = std::make_shared<Registry>();
    const auto find = [registry](std::uint64_t handle) {
        std::lock_guard lock(registry->mutex);
        const auto found = registry->cryptors.find(handle);
        return found == registry->cryptors.end() ? nullptr : found->second;
    };
    // Writes `bytes` to (out, available) and the produced length to *moved.
    const auto deliver = [](GuestMemory& m, const std::vector<std::uint8_t>& bytes, std::uint64_t out,
                            std::uint64_t available, std::uint64_t moved) -> std::int32_t {
        if (moved) m.write64(moved, bytes.size());
        if (bytes.size() > available) return cc_buffer_too_small;
        if (!bytes.empty()) {
            if (!out) return cc_param_error;
            m.write_bytes(out, bytes);
        }
        return cc_success;
    };

    // CCCryptorStatus CCCryptorCreate(CCOperation, CCAlgorithm, CCOptions,
    //     const void* key, size_t keyLength, const void* iv, CCCryptorRef* cryptorRef)
    p.libsystem("_CCCryptorCreate", [&process, registry](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[6]) return status(cc_param_error);
        const auto key = read(m, s.x[3], s.x[4]);
        const auto iv = s.x[5] ? read(m, s.x[5], 16) : std::vector<std::uint8_t>{};
        std::unique_ptr<Cryptor> cryptor;
        const auto result = make_cryptor(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]),
                                         static_cast<std::uint32_t>(s.x[2]), key,
                                         std::span(iv.data(), iv.size()), cryptor);
        if (result != cc_success) { m.write64(s.x[6], 0); return status(result); }
        const auto handle = process.allocate_guest(16, 16);
        m.fill_bytes(handle, 0, 16);
        { std::lock_guard lock(registry->mutex); registry->cryptors.emplace(handle, std::move(cryptor)); }
        m.write64(s.x[6], handle);
        return status(cc_success);
    });
    // CCCryptorStatus CCCryptorUpdate(CCCryptorRef, const void* dataIn, size_t dataInLength,
    //     void* dataOut, size_t dataOutAvailable, size_t* dataOutMoved)
    p.libsystem("_CCCryptorUpdate", [find, deliver](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto cryptor = find(s.x[0]);
        if (!cryptor) return status(cc_param_error);
        const auto input = read(m, s.x[1], s.x[2]);
        if (cryptor->update_length(input.size()) > s.x[4]) {
            if (s.x[5]) m.write64(s.x[5], cryptor->update_length(input.size()));
            return status(cc_buffer_too_small);
        }
        return status(deliver(m, cryptor->update(input), s.x[3], s.x[4], s.x[5]));
    });
    // CCCryptorStatus CCCryptorFinal(CCCryptorRef, void* dataOut, size_t dataOutAvailable, size_t* dataOutMoved)
    p.libsystem("_CCCryptorFinal", [find, deliver](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto cryptor = find(s.x[0]);
        if (!cryptor) return status(cc_param_error);
        if (cryptor->final_length() > s.x[2]) {
            if (s.x[3]) m.write64(s.x[3], cryptor->final_length());
            return status(cc_buffer_too_small);
        }
        std::vector<std::uint8_t> output;
        const auto result = cryptor->finish(output);
        if (result != cc_success) { if (s.x[3]) m.write64(s.x[3], 0); return status(result); }
        return status(deliver(m, output, s.x[1], s.x[2], s.x[3]));
    });
    // size_t CCCryptorGetOutputLength(CCCryptorRef, size_t inputLength, bool final)
    p.libsystem("_CCCryptorGetOutputLength", [find](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto cryptor = find(s.x[0]);
        if (!cryptor) return 0;
        if (!(s.x[2] & 0xff)) return cryptor->update_length(static_cast<std::size_t>(s.x[1]));
        const auto total = cryptor->pending.size() + s.x[1];
        if (cryptor->operation == op_encrypt && (cryptor->options & option_pkcs7)) return (total / cryptor->block + 1) * cryptor->block;
        return total;
    });
    // CCCryptorStatus CCCryptorReset(CCCryptorRef, const void* iv)
    p.libsystem("_CCCryptorReset", [find](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto cryptor = find(s.x[0]);
        if (!cryptor) return status(cc_param_error);
        cryptor->pending.clear();
        if (s.x[1]) m.read_bytes(s.x[1], std::span(cryptor->iv.data(), cryptor->iv.size()));
        else std::fill(cryptor->iv.begin(), cryptor->iv.end(), 0);
        cryptor->restart_stream();
        return status(cc_success);
    });
    p.libsystem("_CCCryptorRelease", [&process, registry](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::shared_ptr<Cryptor> released;
        {
            std::lock_guard lock(registry->mutex);
            const auto found = registry->cryptors.find(s.x[0]);
            if (found == registry->cryptors.end()) return status(cc_param_error);
            released = std::move(found->second);
            registry->cryptors.erase(found);
        }
        process.release_guest(s.x[0]);
        return status(cc_success);
    });
    // CCCryptorStatus CCCrypt(op, alg, options, key, keyLength, iv, dataIn, dataInLength,
    //     dataOut, dataOutAvailable, dataOutMoved): the last three on the stack.
    p.libsystem("_CCCrypt", [deliver](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto out = m.read64(s.sp), available = m.read64(s.sp + 8), moved = m.read64(s.sp + 16);
        const auto key = read(m, s.x[3], s.x[4]);
        const auto iv = s.x[5] ? read(m, s.x[5], 16) : std::vector<std::uint8_t>{};
        std::unique_ptr<Cryptor> cryptor;
        const auto result = make_cryptor(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]),
                                         static_cast<std::uint32_t>(s.x[2]), key, std::span(iv.data(), iv.size()), cryptor);
        if (result != cc_success) return status(result);
        const auto input = read(m, s.x[6], s.x[7]);
        const auto needed = cryptor->operation == op_encrypt && (cryptor->options & option_pkcs7)
            ? (input.size() / cryptor->block + 1) * cryptor->block : input.size();
        if (needed > available) { if (moved) m.write64(moved, needed); return status(cc_buffer_too_small); }
        auto output = cryptor->update(input);
        std::vector<std::uint8_t> tail;
        const auto finished = cryptor->finish(tail);
        if (finished != cc_success) { if (moved) m.write64(moved, 0); return status(finished); }
        output.insert(output.end(), tail.begin(), tail.end());
        return status(deliver(m, output, out, available, moved));
    });
    // CCCryptorStatus CCCryptorCreateWithMode(op, mode, alg, padding, iv, key, keyLength, tweak, [stack] tweakLength,
    //     numRounds, CCModeOptions, CCCryptorRef *): the stack arguments at their natural alignment.
    p.libsystem("_CCCryptorCreateWithMode", [&process, registry](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto out = m.read64(s.sp + 16);
        if (!out) return status(cc_param_error);
        m.write64(out, 0);
        const auto mode = static_cast<std::uint32_t>(s.x[1]), padding = static_cast<std::uint32_t>(s.x[3]);
        if (padding > 1) return status(cc_param_error);
        std::uint32_t options = padding ? option_pkcs7 : 0, stream_mode = 0;
        switch (mode) {
        case mode_ecb: options |= option_ecb; break;
        case mode_cbc: break;
        case mode_cfb: case mode_ctr: case mode_ofb: case mode_cfb8:
            if (padding) return status(cc_param_error);
            stream_mode = mode;
            break;
        default: return status(cc_unimplemented);   // F8, LRW, XTS, RC4, GCM, CCM
        }
        if (mode == mode_ctr && (static_cast<std::uint32_t>(m.read32(s.sp + 12)) & 1)) return status(cc_unimplemented);   // the little-endian counter
        if (m.read64(s.sp) != 0 || s.x[7]) return status(cc_param_error);   // tweaks are XTS's
        const auto key = read(m, s.x[5], s.x[6]);
        const auto iv = s.x[4] ? read(m, s.x[4], s.x[2] == 0 ? 16 : 8) : std::vector<std::uint8_t>{};   // AES: 16, DES / 3DES: 8
        std::unique_ptr<Cryptor> cryptor;
        const auto result = make_cryptor(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[2]), options, key,
                                         std::span(iv.data(), iv.size()), cryptor, stream_mode);
        if (result != cc_success) return status(result);
        const auto handle = process.allocate_guest(16, 16);
        m.fill_bytes(handle, 0, 16);
        { std::lock_guard lock(registry->mutex); registry->cryptors.emplace(handle, std::move(cryptor)); }
        m.write64(out, handle);
        return status(cc_success);
    });

    // AES-GCM one-shots. CCCryptorGCMOneshotEncrypt(alg, key, keyLength, iv, ivLen, aData, aDataLen, dataIn,
    //     [stack] dataInLength, cipherDataOut, tagOut, tagLength); Decrypt takes the tag in and checks it.
    const auto gcm_oneshot = [](CpuState& s, GuestMemory& m, bool encrypt) -> std::int32_t {
        if (s.x[0] != 0) return cc_param_error;   // kCCAlgorithmAES
        const auto length = m.read64(s.sp), out = m.read64(s.sp + 8), tag_address = m.read64(s.sp + 16), tag_length = m.read64(s.sp + 24);
        if (tag_length < 1 || tag_length > 16 || !tag_address || (length && !out)) return cc_param_error;
        const auto key = read(m, s.x[1], s.x[2]), iv = read(m, s.x[3], s.x[4]), aad = read(m, s.x[5], s.x[6]), input = read(m, s.x[7], length);
        std::vector<std::uint8_t> output;
        Block tag{};
        if (const auto result = gcm(encrypt, key, iv, aad, input, output, tag)) return result;
        if (encrypt) {
            m.write_bytes(tag_address, std::span(tag.data(), static_cast<std::size_t>(tag_length)));
        } else {
            std::vector<std::uint8_t> expected(static_cast<std::size_t>(tag_length));
            m.read_bytes(tag_address, expected);
            std::uint8_t difference = 0;
            for (std::size_t index = 0; index < expected.size(); ++index) difference |= static_cast<std::uint8_t>(expected[index] ^ tag[index]);
            if (difference) return cc_unspecified_error;   // authentication failed: no plaintext
        }
        if (!output.empty()) m.write_bytes(out, output);
        return cc_success;
    };
    p.libsystem("_CCCryptorGCMOneshotEncrypt", [gcm_oneshot](CpuState& s, GuestMemory& m) -> std::uint64_t { return status(gcm_oneshot(s, m, true)); });
    p.libsystem("_CCCryptorGCMOneshotDecrypt", [gcm_oneshot](CpuState& s, GuestMemory& m) -> std::uint64_t { return status(gcm_oneshot(s, m, false)); });
    // CCCryptorGCM(op, alg, key, keyLength, iv, ivLen, aData, aDataLen, [stack] dataIn, dataInLength, dataOut,
    //     tagOut, size_t *tagLength): the tag is computed for both operations (callers compare it after decryption).
    p.libsystem("_CCCryptorGCM", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if ((s.x[0] != op_encrypt && s.x[0] != op_decrypt) || s.x[1] != 0) return status(cc_param_error);
        const auto in = m.read64(s.sp), length = m.read64(s.sp + 8), out = m.read64(s.sp + 16), tag_address = m.read64(s.sp + 24),
                   tag_length_address = m.read64(s.sp + 32);
        if (!tag_address || !tag_length_address || (length && !out)) return status(cc_param_error);
        const auto tag_length = m.read64(tag_length_address);
        if (tag_length < 1 || tag_length > 16) return status(cc_param_error);
        const auto key = read(m, s.x[2], s.x[3]), iv = read(m, s.x[4], s.x[5]), aad = read(m, s.x[6], s.x[7]), input = read(m, in, length);
        std::vector<std::uint8_t> output;
        Block tag{};
        if (const auto result = gcm(s.x[0] == op_encrypt, key, iv, aad, input, output, tag)) return status(result);
        if (!output.empty()) m.write_bytes(out, output);
        m.write_bytes(tag_address, std::span(tag.data(), static_cast<std::size_t>(tag_length)));
        return status(cc_success);
    });

    // RFC 3394 AES key wrap. The default initial value: const uint8_t *const CCrfc3394_iv, const size_t CCrfc3394_ivLen.
    static constexpr std::array<std::uint8_t, 8> rfc3394_iv{0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6};
    {
        const auto bytes = process.allocate_guest(8, 8);
        process.main_engine().write_bytes(bytes, rfc3394_iv);
        std::array<std::uint8_t, 8> pointer{};
        for (int index = 0; index < 8; ++index) pointer[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(bytes >> (8 * index));
        static_cast<void>(p.data(libsystem_library, "_CCrfc3394_iv", 8, pointer));
        static_cast<void>(p.data(libsystem_library, "_CCrfc3394_ivLen", 8, std::array<std::uint8_t, 8>{8, 0, 0, 0, 0, 0, 0, 0}));
    }
    // (algorithm, iv, ivLen, kek, kekLen, input, inputLen, output, [stack] size_t *outputLen)
    const auto key_wrap = [](CpuState& s, GuestMemory& m, bool wrap) -> std::int32_t {
        if (s.x[0] != 1) return cc_param_error;   // kCCWRAPAES
        const auto length_address = m.read64(s.sp);
        if (!s.x[7] || !length_address) return cc_param_error;
        const auto iv = s.x[1] ? read(m, s.x[1], s.x[2]) : std::vector<std::uint8_t>(rfc3394_iv.begin(), rfc3394_iv.end());
        const auto kek = read(m, s.x[3], s.x[4]), input = read(m, s.x[5], s.x[6]);
        std::vector<std::uint8_t> output;
        if (const auto result = aes_key_wrap(wrap, kek, iv, input, output)) return result;
        if (m.read64(length_address) < output.size()) { m.write64(length_address, output.size()); return cc_buffer_too_small; }
        m.write_bytes(s.x[7], output);
        m.write64(length_address, output.size());
        return cc_success;
    };
    p.libsystem("_CCSymmetricKeyWrap", [key_wrap](CpuState& s, GuestMemory& m) -> std::uint64_t { return status(key_wrap(s, m, true)); });
    p.libsystem("_CCSymmetricKeyUnwrap", [key_wrap](CpuState& s, GuestMemory& m) -> std::uint64_t { return status(key_wrap(s, m, false)); });
    p.libsystem("_CCSymmetricWrappedSize", [](CpuState& s, GuestMemory&) -> std::uint64_t { return s.x[0] == 1 ? s.x[1] + 8 : 0; });
    p.libsystem("_CCSymmetricUnwrappedSize", [](CpuState& s, GuestMemory&) -> std::uint64_t { return s.x[0] == 1 && s.x[1] >= 8 ? s.x[1] - 8 : 0; });

    // int CCDigest(CCDigestAlgorithm, const uint8_t *data, size_t length, uint8_t *output); size_t CCDigestGetOutputSize(algorithm).
    p.libsystem("_CCDigest", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::size_t size{};
        const auto digest = digest_of(static_cast<std::uint32_t>(s.x[0]), size);
        if (!digest) return status(s.x[0] == 0 ? cc_param_error : cc_unimplemented);
        if (!s.x[3]) return status(cc_param_error);
        const auto input = read(m, s.x[1], s.x[2]);
        const auto value = host::crypto::hash(*digest, std::as_bytes(std::span(input)));
        if (!value) return status(cc_unimplemented);
        m.write_bytes(s.x[3], *value);
        return status(cc_success);
    });
    p.libsystem("_CCDigestGetOutputSize", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::size_t size{};
        static_cast<void>(digest_of(static_cast<std::uint32_t>(s.x[0]), size));
        return size;
    });
    // uint CCCalibratePBKDF(CCPBKDFAlgorithm, size_t passwordLen, size_t saltLen, CCPseudoRandomAlgorithm prf,
    //     size_t derivedKeyLen, uint32_t msec): the rounds that take about `msec` here, measured.
    p.libsystem("_CCCalibratePBKDF", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (static_cast<std::uint32_t>(s.x[0]) != 2 || !s.x[4] || s.x[4] > 4096 || s.x[1] > 4096 || s.x[2] > 4096) return 0xffffffffULL;
        const std::vector<std::uint8_t> password(static_cast<std::size_t>(s.x[1]), 'p'), salt(static_cast<std::size_t>(s.x[2]), 's');
        std::vector<std::uint8_t> output(static_cast<std::size_t>(s.x[4]));
        constexpr std::uint32_t probe = 10000;
        const auto start = std::chrono::steady_clock::now();
        if (test_pbkdf2(static_cast<std::uint32_t>(s.x[3]), password, salt, probe, output) != cc_success) return 0xffffffffULL;
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto wanted = static_cast<double>(static_cast<std::uint32_t>(s.x[5])) / 1000.0;
        const auto rounds = seconds > 0 ? wanted / seconds * probe : 1e9;
        return static_cast<std::uint64_t>(std::clamp(rounds, 1.0, 4294967294.0));
    });

    // int CCKeyDerivationPBKDF(CCPBKDFAlgorithm, const char* password, size_t passwordLen,
    //     const uint8_t* salt, size_t saltLen, CCPseudoRandomAlgorithm prf, unsigned rounds,
    //     uint8_t* derivedKey, size_t derivedKeyLen /* stack */)
    p.libsystem("_CCKeyDerivationPBKDF", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (static_cast<std::uint32_t>(s.x[0]) != 2) return status(cc_param_error); // kCCPBKDF2
        const auto length = m.read64(s.sp);
        if (!s.x[7] || !length || length > (1U << 20)) return status(cc_param_error);
        const auto password = read(m, s.x[1], s.x[2]), salt = read(m, s.x[3], s.x[4]);
        std::vector<std::uint8_t> output(static_cast<std::size_t>(length));
        const auto result = test_pbkdf2(static_cast<std::uint32_t>(s.x[5]), password, salt, static_cast<std::uint32_t>(s.x[6]), output);
        if (result == cc_success) m.write_bytes(s.x[7], output);
        return status(result);
    });
}

} // namespace alchemy::export_fill
