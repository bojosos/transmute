#ifndef ALCHEMY_QLAB_COREMEDIA_H
#define ALCHEMY_QLAB_COREMEDIA_H
/* CoreMedia's time types (CMTime, CMTimeRange) as the Swift ClangImporter
   sees them, for Alchemy's project-authored CoreMedia overlay
   (graphics/swift/CoreMedia). Independently authored from the published
   CoreMedia interface; no SDK content. */
#include <stdint.h>

typedef int64_t CMTimeValue;
typedef int32_t CMTimeScale;
typedef int64_t CMTimeEpoch;

typedef enum __attribute__((flag_enum, enum_extensibility(open))) CMTimeFlags : uint32_t CMTimeFlags;
enum CMTimeFlags : uint32_t {
    kCMTimeFlags_Valid = 1U << 0,
    kCMTimeFlags_HasBeenRounded = 1U << 1,
    kCMTimeFlags_PositiveInfinity = 1U << 2,
    kCMTimeFlags_NegativeInfinity = 1U << 3,
    kCMTimeFlags_Indefinite = 1U << 4,
    kCMTimeFlags_ImpliedValueFlagsMask = kCMTimeFlags_PositiveInfinity | kCMTimeFlags_NegativeInfinity | kCMTimeFlags_Indefinite,
};

/* A rational time value: value / timescale seconds, with flags and an epoch. */
typedef struct CMTime {
    CMTimeValue value;
    CMTimeScale timescale;
    CMTimeFlags flags;
    CMTimeEpoch epoch;
} CMTime;

/* A time range: a start time and a duration. */
typedef struct CMTimeRange {
    CMTime start;
    CMTime duration;
} CMTimeRange;

#endif
