#include "alchemy/darwin/libsystem/services_impl.hpp"

// Streams without a descriptor (StreamBackend, services_impl.hpp):
//   fmemopen(buf, size, mode)       a fixed buffer of size bytes (allocated and freed with the
//                                   stream when buf is NULL). "r" reads the size bytes, "w"
//                                   starts empty, "a" at the first NUL; "+" allows both. Unless
//                                   the mode has "b", the contents stay NUL-terminated when
//                                   there is room. Writes past the end are short (ENOSPC).
//   open_memstream(bufp, sizep)     a write stream into a malloc'd buffer that grows (through the
//   open_wmemstream(bufp, sizep)    guest's realloc, so the caller frees it); *bufp and *sizep
//                                   follow every write, seek, fflush and fclose. The wide form
//                                   stores wchar_t: the bytes written are UTF-8, decoded.
//   funopen(cookie, read, write, seek, close)  the guest's functions, called on the calling
//                                   thread with the cookie; fropen/fwopen are macros over it.
namespace alchemy {

namespace {

constexpr std::uint32_t enospc = 28, espipe = 29, eilseq = 92;

// The calling guest thread's errno (a callback's failure), or `fallback`.
std::uint32_t guest_errno(ProcessContext& context, std::uint32_t fallback) {
    const auto thread = context.current_thread();
    if (!thread || !thread->errno_address) return fallback;
    const auto value = context.calling_engine().read32(thread->errno_address);
    return value ? value : fallback;
}

std::uint64_t guest_call(ProcessContext& context, std::string_view name, std::initializer_list<std::uint64_t> arguments) {
    const auto found = context.resolve(system_library, name);
    if (!found) throw std::runtime_error("Darwin memory streams require " + std::string(name.substr(1)));
    return EngineAdapter(context.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
}

class FixedBuffer final : public StreamBackend {
public:
    FixedBuffer(ProcessContext& context, std::uint64_t buffer, std::uint64_t capacity, bool owned, bool readable, bool writable,
                bool append, bool binary, std::uint64_t size)
        : context_(context), buffer_(buffer), capacity_(capacity), size_(size), owned_(owned), readable_(readable),
          writable_(writable), append_(append), binary_(binary) {
        if (append) position_ = size;
    }
    std::uint64_t read(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size) override {
        if (!readable_) throw ServiceError{ebadf};
        const auto count = position_ >= size_ ? 0 : std::min(size, size_ - position_);
        if (count) memory.copy_bytes(address, buffer_ + position_, count);
        position_ += count;
        return count;
    }
    std::uint64_t write(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size) override {
        if (!writable_) throw ServiceError{ebadf};
        if (append_) position_ = size_;
        if (!size) return 0;
        if (position_ >= capacity_) throw ServiceError{enospc};
        const auto count = std::min(size, capacity_ - position_);
        memory.copy_bytes(buffer_ + position_, address, count);
        position_ += count;
        size_ = std::max(size_, position_);
        if (!binary_ && size_ < capacity_) memory.write8(buffer_ + size_, 0);
        return count;
    }
    std::uint64_t seek(transmute::GuestMemory&, std::int64_t offset, int whence) override {
        const auto base = static_cast<std::int64_t>(whence == 0 ? 0 : whence == 1 ? position_ : size_);
        const auto target = base + offset;
        if (target < 0 || static_cast<std::uint64_t>(target) > capacity_) throw ServiceError{einval};
        position_ = static_cast<std::uint64_t>(target);
        return position_;
    }
    std::uint64_t close(transmute::GuestMemory&) override {
        if (owned_) static_cast<void>(guest_call(context_, "_free", {buffer_}));
        return 0;
    }

private:
    ProcessContext& context_;
    std::uint64_t buffer_, capacity_, size_, position_{};
    bool owned_, readable_, writable_, append_, binary_;
};

class GrowingBuffer final : public StreamBackend {
public:
    GrowingBuffer(ProcessContext& context, transmute::GuestMemory& memory, std::uint64_t buffer_cell, std::uint64_t size_cell, bool wide)
        : context_(context), buffer_cell_(buffer_cell), size_cell_(size_cell), element_(wide ? 4 : 1) {
        reserve(memory, 64);
        publish(memory);
    }
    std::uint64_t read(transmute::GuestMemory&, std::uint64_t, std::uint64_t) override { throw ServiceError{ebadf}; }
    std::uint64_t write(transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size) override {
        if (element_ == 1) {
            place(memory, size);
            memory.copy_bytes(buffer_ + position_, address, size);
            advance(memory, size);
            return size;
        }
        // Wide: whole UTF-8 sequences become wchar_t; a partial one waits for the next write.
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (size) memory.read_bytes(address, bytes);
        pending_.insert(pending_.end(), bytes.begin(), bytes.end());
        std::vector<std::uint32_t> characters;
        std::size_t at = 0;
        while (at < pending_.size()) {
            const auto lead = pending_[at];
            const std::size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 0;
            if (!length) { pending_.clear(); throw ServiceError{eilseq}; }
            if (at + length > pending_.size()) break;
            std::uint32_t value = length == 1 ? lead : lead & (0x7fU >> length);
            for (std::size_t index = 1; index < length; ++index) {
                const auto next = pending_[at + index];
                if ((next & 0xc0) != 0x80) { pending_.clear(); throw ServiceError{eilseq}; }
                value = (value << 6) | (next & 0x3fU);
            }
            characters.push_back(value);
            at += length;
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(at));
        place(memory, characters.size());
        for (std::size_t index = 0; index < characters.size(); ++index) memory.write32(buffer_ + (position_ + index) * 4, characters[index]);
        advance(memory, characters.size());
        return size;
    }
    std::uint64_t seek(transmute::GuestMemory& memory, std::int64_t offset, int whence) override {
        const auto base = static_cast<std::int64_t>(whence == 0 ? 0 : whence == 1 ? position_ : length_);
        const auto target = base + offset;
        if (target < 0) throw ServiceError{einval};
        position_ = static_cast<std::uint64_t>(target);
        publish(memory);
        return position_;
    }
    void flush(transmute::GuestMemory& memory) override { publish(memory); }
    std::uint64_t close(transmute::GuestMemory& memory) override { publish(memory); return 0; }

private:
    void reserve(transmute::GuestMemory& memory, std::uint64_t elements) {
        if (buffer_ && elements + 1 <= capacity_) return;
        auto capacity = std::max<std::uint64_t>(capacity_, 64);
        while (capacity < elements + 1) capacity *= 2;
        const auto grown = guest_call(context_, "_realloc", {buffer_, capacity * element_});
        if (!grown) throw ServiceError{enomem};
        memory.fill_bytes(grown + capacity_ * element_, 0, (capacity - capacity_) * element_);
        buffer_ = grown;
        capacity_ = capacity;
    }
    // Room for `count` elements at the position (a seek past the end leaves zeros between).
    void place(transmute::GuestMemory& memory, std::uint64_t count) {
        reserve(memory, position_ + count);
        if (position_ > length_) memory.fill_bytes(buffer_ + length_ * element_, 0, (position_ - length_) * element_);
    }
    void advance(transmute::GuestMemory& memory, std::uint64_t count) {
        position_ += count;
        length_ = std::max(length_, position_);
        if (element_ == 1) memory.write8(buffer_ + length_, 0); else memory.write32(buffer_ + length_ * 4, 0);
        publish(memory);
    }
    void publish(transmute::GuestMemory& memory) {
        memory.write64(buffer_cell_, buffer_);
        memory.write64(size_cell_, std::min(length_, position_));
    }
    ProcessContext& context_;
    std::uint64_t buffer_cell_, size_cell_, element_;
    std::uint64_t buffer_{}, capacity_{}, length_{}, position_{};
    std::vector<std::uint8_t> pending_;
};

class Callbacks final : public StreamBackend {
public:
    Callbacks(ProcessContext& context, std::uint64_t cookie, std::uint64_t read, std::uint64_t write, std::uint64_t seek, std::uint64_t close)
        : context_(context), cookie_(cookie), read_(read), write_(write), seek_(seek), close_(close) {}
    std::uint64_t read(transmute::GuestMemory&, std::uint64_t address, std::uint64_t size) override {
        if (!read_) throw ServiceError{ebadf};
        return transfer(read_, address, size);
    }
    std::uint64_t write(transmute::GuestMemory&, std::uint64_t address, std::uint64_t size) override {
        if (!write_) throw ServiceError{ebadf};
        return transfer(write_, address, size);
    }
    // fpos_t (*seekfn)(void* cookie, fpos_t offset, int whence)
    std::uint64_t seek(transmute::GuestMemory&, std::int64_t offset, int whence) override {
        if (!seek_) throw ServiceError{espipe};
        const std::array<std::uint64_t, 3> arguments{cookie_, std::bit_cast<std::uint64_t>(offset), static_cast<std::uint64_t>(whence)};
        const auto result = static_cast<std::int64_t>(EngineAdapter(context_.calling_engine()).call(seek_, arguments));
        if (result < 0) throw ServiceError{guest_errno(context_, espipe)};
        return static_cast<std::uint64_t>(result);
    }
    std::uint64_t close(transmute::GuestMemory&) override {
        if (!close_) return 0;
        const std::array<std::uint64_t, 1> arguments{cookie_};
        if (static_cast<std::int32_t>(EngineAdapter(context_.calling_engine()).call(close_, arguments)) < 0)
            throw ServiceError{guest_errno(context_, eio)};
        return 0;
    }

private:
    // int (*fn)(void* cookie, char* buffer, int count)
    std::uint64_t transfer(std::uint64_t function, std::uint64_t address, std::uint64_t size) {
        const auto count = std::min<std::uint64_t>(size, INT32_MAX);
        const std::array<std::uint64_t, 3> arguments{cookie_, address, count};
        const auto result = static_cast<std::int32_t>(EngineAdapter(context_.calling_engine()).call(function, arguments));
        if (result < 0) throw ServiceError{guest_errno(context_, eio)};
        return std::min<std::uint64_t>(static_cast<std::uint64_t>(result), count);
    }
    ProcessContext& context_;
    std::uint64_t cookie_, read_, write_, seek_, close_;
};

} // namespace

void DarwinServices::Impl::install_memory_streams() {
    // FILE* fmemopen(void* buf, size_t size, const char* mode)
    install("_fmemopen", [this](auto& s, auto& m) {
        const auto mode = text(m, s.x[2], 16);
        if (mode.empty() || std::string_view("rwa").find(mode[0]) == std::string_view::npos || !s.x[1]) throw ServiceError{einval};
        const bool plus = mode.find('+') != std::string::npos, binary = mode.find('b') != std::string::npos;
        const bool readable = mode[0] == 'r' || plus, writable = mode[0] != 'r' || plus;
        auto buffer = s.x[0];
        const bool owned = !buffer;
        if (owned) {
            buffer = guest_call(context, "_calloc", {1, s.x[1]});
            if (!buffer) throw ServiceError{enomem};
        } else validate(m, buffer, s.x[1], writable);
        std::uint64_t size = s.x[1];
        if (mode[0] == 'w') { size = 0; m.write8(buffer, 0); }
        else if (mode[0] == 'a') {
            size = 0;
            while (size < s.x[1] && m.read8(buffer + size)) ++size;
        }
        try {
            return new_backend_stream(std::make_shared<FixedBuffer>(context, buffer, s.x[1], owned, readable, writable, mode[0] == 'a', binary, size),
                                      readable, writable);
        } catch (...) { if (owned) static_cast<void>(guest_call(context, "_free", {buffer})); throw; }
    }, 0);
    // FILE* open_memstream(char** bufp, size_t* sizep) / open_wmemstream(wchar_t** bufp, size_t* sizep)
    for (const auto wide : {false, true})
        install(wide ? "_open_wmemstream" : "_open_memstream", [this, wide](auto& s, auto& m) {
            if (!s.x[0] || !s.x[1]) throw ServiceError{einval};
            validate(m, s.x[0], 8, true);
            validate(m, s.x[1], 8, true);
            return new_backend_stream(std::make_shared<GrowingBuffer>(context, m, s.x[0], s.x[1], wide), false, true);
        }, 0);
    // FILE* funopen(const void* cookie, readfn, writefn, seekfn, closefn)
    install("_funopen", [this](auto& s, auto&) {
        if (!s.x[1] && !s.x[2]) throw ServiceError{einval};
        return new_backend_stream(std::make_shared<Callbacks>(context, s.x[0], s.x[1], s.x[2], s.x[3], s.x[4]), s.x[1] != 0, s.x[2] != 0);
    }, 0);
}

} // namespace alchemy
