// fx corpus ARM64 CRT: C++ pieces that live in the static vcruntime.lib
// (operator new/delete and type_info's destructor). std::nothrow and the other
// static msvcprt.lib parts are compiled from the MSVC STL sources.
#include <malloc.h>
#include <new.h>
#include <stdint.h>
#include <stdlib.h>

#include <new>
#include <typeinfo>

type_info::~type_info() noexcept {}

// type_info::name() caches undecorated names in this list (vcruntime_typeinfo.h
// only forward-declares the node type). vcruntime treats it as an SLIST_HEADER
// (Interlocked{Push,Pop}EntrySList), which must be 16-byte aligned.
struct alignas(16) __type_info_node {
    void* _MemPtr;
    __type_info_node* _Next;
};
__type_info_node __type_info_root_node{};

// Slot 0 of type_info's vftable (arm64_helpers.S). type_info objects are
// compiler-generated statics and are never deleted.
extern "C" void* fx_type_info_deleting_dtor(void* self, unsigned) {
    return self;
}

namespace {
[[noreturn]] void fx_throw_bad_alloc(size_t size) {
    if (size == SIZE_MAX) throw std::bad_array_new_length{};
    throw std::bad_alloc{};
}
} // namespace

void* __cdecl operator new(size_t size) {
    for (;;) {
        if (void* block = malloc(size ? size : 1)) return block;
        if (_callnewh(size) == 0) fx_throw_bad_alloc(size);
    }
}

void* __cdecl operator new[](size_t size) {
    return operator new(size);
}

void* __cdecl operator new(size_t size, const std::nothrow_t&) noexcept {
    try {
        return operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* __cdecl operator new[](size_t size, const std::nothrow_t&) noexcept {
    try {
        return operator new[](size);
    } catch (...) {
        return nullptr;
    }
}

void __cdecl operator delete(void* block) noexcept {
    free(block);
}
void __cdecl operator delete[](void* block) noexcept {
    free(block);
}
void __cdecl operator delete(void* block, size_t) noexcept {
    free(block);
}
void __cdecl operator delete[](void* block, size_t) noexcept {
    free(block);
}
void __cdecl operator delete(void* block, const std::nothrow_t&) noexcept {
    free(block);
}
void __cdecl operator delete[](void* block, const std::nothrow_t&) noexcept {
    free(block);
}

void* __cdecl operator new(size_t size, std::align_val_t align) {
    for (;;) {
        if (void* block = _aligned_malloc(size ? size : 1, static_cast<size_t>(align))) return block;
        if (_callnewh(size) == 0) fx_throw_bad_alloc(size);
    }
}
void* __cdecl operator new[](size_t size, std::align_val_t align) {
    return operator new(size, align);
}
void* __cdecl operator new(size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    try {
        return operator new(size, align);
    } catch (...) {
        return nullptr;
    }
}
void* __cdecl operator new[](size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    try {
        return operator new[](size, align);
    } catch (...) {
        return nullptr;
    }
}
void __cdecl operator delete(void* block, std::align_val_t) noexcept {
    _aligned_free(block);
}
void __cdecl operator delete[](void* block, std::align_val_t) noexcept {
    _aligned_free(block);
}
void __cdecl operator delete(void* block, size_t, std::align_val_t) noexcept {
    _aligned_free(block);
}
void __cdecl operator delete[](void* block, size_t, std::align_val_t) noexcept {
    _aligned_free(block);
}
void __cdecl operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    _aligned_free(block);
}
void __cdecl operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    _aligned_free(block);
}
