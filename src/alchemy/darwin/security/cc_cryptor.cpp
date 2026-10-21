#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/host/crypto.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

// CommonCrypto symmetric ciphers and PBKDF2 (libcommonCrypto, re-exported by
// libSystem), imported by an asset-browser service: CCCryptorCreate/
// Update/Final/Release/GetOutputLength/Reset, the one-shot CCCrypt and
// CCKeyDerivationPBKDF. Digests, HMAC and random bytes are the
// network/security provider's (src/alchemy/security).
//
// Block ciphers run on the host's (host::crypto::BlockCipher: AES, DES, 3DES)
// in ECB mode, one block at a time; CBC chaining and PKCS#7 padding are done here so streaming Update calls
// behave as CommonCrypto's (a decryptor with padding keeps the last block back
// until Final). Other algorithms (CAST, RC4, RC2, Blowfish) and PBKDF2 with
// HMAC-SHA224 report kCCUnimplemented.
namespace alchemy::export_fill {
namespace {

constexpr std::int32_t cc_success = 0, cc_param_error = -4300, cc_buffer_too_small = -4301, cc_alignment_error = -4303,
    cc_decode_error = -4304, cc_unimplemented = -4305, cc_key_size_error = -4310;
constexpr std::uint32_t op_encrypt = 0, op_decrypt = 1;
constexpr std::uint32_t option_pkcs7 = 1, option_ecb = 2;

std::uint64_t status(std::int32_t value) { return static_cast<std::uint32_t>(value); }

struct Cryptor {
    std::uint32_t operation{}, options{};
    std::size_t block{};
    std::vector<std::uint8_t> iv, initial_iv, pending;
    host::crypto::BlockCipher cipher;

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
        auto total = pending.size() + length;
        auto blocks = total / block;
        if (holds_back() && blocks && total % block == 0) --blocks;
        return blocks * block;
    }
    std::size_t final_length() const {
        if (operation == op_encrypt) return (options & option_pkcs7) ? (pending.size() / block + 1) * block : pending.size();
        return pending.size();
    }
    std::vector<std::uint8_t> update(std::span<const std::uint8_t> input) {
        pending.insert(pending.end(), input.begin(), input.end());
        const auto produce = update_length(0);
        std::vector<std::uint8_t> output(produce);
        for (std::size_t offset = 0; offset < produce; offset += block) process_block(pending.data() + offset, output.data() + offset);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(produce));
        return output;
    }
    std::int32_t finish(std::vector<std::uint8_t>& output) {
        output.clear();
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
                          std::span<const std::uint8_t> key, std::span<const std::uint8_t> iv, std::unique_ptr<Cryptor>& out) {
    if (operation != op_encrypt && operation != op_decrypt) return cc_param_error;
    if (options & ~(option_pkcs7 | option_ecb)) return cc_param_error;
    auto cryptor = std::make_unique<Cryptor>();
    cryptor->operation = operation;
    cryptor->options = options;
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
    out = std::move(cryptor);
    return cc_success;
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
