#ifndef ALCHEMY_QLAB_COREAUDIO_H
#define ALCHEMY_QLAB_COREAUDIO_H
/* CoreAudio's buffer types (CoreAudioTypes) as the Swift ClangImporter sees
   them, for Alchemy's project-authored CoreAudio overlay
   (graphics/swift/CoreAudio). Independently authored from the published
   CoreAudio interface; no SDK content. calloc is libSystem's: the overlay
   allocates AudioBufferLists that callers release with free(), as
   documented. */
#include <stddef.h>
#include <stdint.h>

/* One buffer of audio data and the number of interleaved channels in it. */
typedef struct AudioBuffer {
    uint32_t mNumberChannels;
    uint32_t mDataByteSize;
    void* _Nullable mData;
} AudioBuffer;

/* A variable-length array of AudioBuffer structures (mNumberBuffers entries). */
typedef struct AudioBufferList {
    uint32_t mNumberBuffers;
    AudioBuffer mBuffers[1];
} AudioBufferList;

void* _Nullable calloc(size_t count, size_t size) __attribute__((swift_private));

#endif
