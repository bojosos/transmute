// malloc/calloc/realloc/free stress with pattern verification, plus aligned
// allocation and private Win32 heaps.
#include <windows.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 2048
#define ROUNDS 150000

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

typedef struct {
    unsigned char* ptr;
    size_t size;
    unsigned char seed;
} Slot;

static Slot slots[SLOTS];
static unsigned long long errors;

static void fill(unsigned char* p, size_t from, size_t to, unsigned char seed) {
    for (size_t i = from; i < to; i++) p[i] = (unsigned char)(seed + i * 31);
}

// Checks every 61st byte and the last one.
static void verify(const Slot* s, size_t upto) {
    if (upto && s->ptr[upto - 1] != (unsigned char)(s->seed + (upto - 1) * 31)) errors++;
    for (size_t i = 0; i < upto; i += 61) {
        if (s->ptr[i] != (unsigned char)(s->seed + i * 31)) {
            errors++;
            return;
        }
    }
}

static size_t pick_size(void) {
    uint32_t r = rng();
    switch (r & 15) {
    case 0: return 0;
    case 1: return (r >> 8) % 8 + 1;
    case 2: case 3: case 4: case 5: return (r >> 8) % 128;
    case 6: case 7: case 8: return (r >> 8) % 1024;
    case 9: case 10: return (r >> 8) % 16384;
    case 11: return (r & 0x30) == 0 ? 65536 + (r >> 8) % 200000 : (r >> 8) % 2048; // large: VirtualAlloc-backed
    default: return (r >> 8) % 4096;
    }
}

int main(void) {
    unsigned long long mallocs = 0, callocs = 0, reallocs = 0, frees = 0, bytes = 0, checksum = 0;
    for (int round = 0; round < ROUNDS; round++) {
        uint32_t r = rng();
        Slot* s = &slots[r % SLOTS];
        switch ((r >> 12) % 5) {
        case 0:
        case 1:
            if (s->ptr) {
                verify(s, s->size);
                free(s->ptr);
                frees++;
            }
            s->size = pick_size();
            s->ptr = (unsigned char*)malloc(s->size);
            s->seed = (unsigned char)r;
            if (!s->ptr && s->size) errors++;
            if (s->ptr) fill(s->ptr, 0, s->size, s->seed);
            mallocs++;
            bytes += s->size;
            break;
        case 2:
            if (s->ptr) {
                verify(s, s->size);
                free(s->ptr);
                frees++;
            }
            s->size = pick_size() / 4;
            s->ptr = (unsigned char*)calloc(s->size, 1);
            s->seed = (unsigned char)(r >> 3);
            for (size_t i = 0; i < s->size; i++) {
                if (s->ptr[i] != 0) {
                    errors++;
                    break;
                }
            }
            fill(s->ptr, 0, s->size, s->seed);
            callocs++;
            bytes += s->size;
            break;
        case 3: {
            if (!s->ptr) break;
            size_t new_size = pick_size() + 1;
            unsigned char* p = (unsigned char*)realloc(s->ptr, new_size);
            if (!p) {
                errors++;
                break;
            }
            s->ptr = p;
            size_t keep = s->size < new_size ? s->size : new_size;
            verify(s, keep);
            fill(s->ptr, keep, new_size, s->seed);
            s->size = new_size;
            reallocs++;
            bytes += new_size;
            break;
        }
        default:
            if (s->ptr) {
                verify(s, s->size);
                for (size_t i = 0; i < s->size; i += 61) checksum = checksum * 131 + s->ptr[i];
                free(s->ptr);
                s->ptr = NULL;
                s->size = 0;
                frees++;
            }
            break;
        }
    }
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].ptr) {
            verify(&slots[i], slots[i].size);
            free(slots[i].ptr);
            frees++;
        }
    }
    printf("malloc=%llu calloc=%llu realloc=%llu free=%llu bytes=%llu\n", mallocs, callocs, reallocs, frees, bytes);
    printf("checksum=%016llx errors=%llu\n", checksum, errors);

    unsigned misaligned = 0;
    for (size_t align = 8; align <= 65536; align *= 2) {
        void* blocks[16];
        for (int i = 0; i < 16; i++) {
            blocks[i] = _aligned_malloc(align * (size_t)(i + 1) / 3 + 1, align);
            if (((uintptr_t)blocks[i] & (align - 1)) != 0) misaligned++;
            memset(blocks[i], 0xA5, align * (size_t)(i + 1) / 3 + 1);
        }
        for (int i = 0; i < 16; i++) blocks[i] = _aligned_realloc(blocks[i], align * 2, align);
        for (int i = 0; i < 16; i++) {
            if (((uintptr_t)blocks[i] & (align - 1)) != 0) misaligned++;
            _aligned_free(blocks[i]);
        }
    }
    printf("aligned misaligned=%u\n", misaligned);

    void* sized = malloc(1000);
    printf("_msize=%zu\n", _msize(sized));
    free(sized);

    HANDLE heap = HeapCreate(0, 0, 0);
    unsigned long long heap_sum = 0;
    void* hblocks[256];
    for (int i = 0; i < 256; i++) {
        hblocks[i] = HeapAlloc(heap, HEAP_ZERO_MEMORY, (SIZE_T)(i * 37 + 1));
        ((unsigned char*)hblocks[i])[i * 37] = (unsigned char)i;
    }
    for (int i = 0; i < 256; i += 2) hblocks[i] = HeapReAlloc(heap, HEAP_ZERO_MEMORY, hblocks[i], (SIZE_T)(i * 53 + 7));
    for (int i = 0; i < 256; i++) {
        heap_sum += HeapSize(heap, 0, hblocks[i]);
        heap_sum += ((unsigned char*)hblocks[i])[i * 37];
        HeapFree(heap, 0, hblocks[i]);
    }
    int valid = HeapValidate(heap, 0, NULL) != 0;
    int destroyed = HeapDestroy(heap) != 0;
    printf("private heap sum=%llu validate=%d destroy=%d\n", heap_sum, valid, destroyed);

    // Larger than the address space. The volatile sink keeps the compiler from
    // eliding the allocation and folding the NULL check.
    static void* volatile huge_sink;
    huge_sink = malloc((size_t)1 << 62);
    printf("huge malloc null=%d errno=%d\n", huge_sink == NULL, errno);
    free(huge_sink);
    fflush(stdout);
    return 0;
}
