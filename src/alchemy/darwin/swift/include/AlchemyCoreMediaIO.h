#ifndef ALCHEMY_COREMEDIAIO_H
#define ALCHEMY_COREMEDIAIO_H
/* The CoreMediaIO camera-extension classes the Swift CoreMediaIO overlay
   extends, as the Swift ClangImporter sees them, for Alchemy's
   project-authored overlay (src/alchemy/darwin/swift/CoreMediaIO).
   Independently authored from Apple's public CoreMediaIO documentation; no
   SDK content. The declarations the overlay refines are NS_REFINED_FOR_SWIFT
   (Swift sees them with a leading "__"). */
#import <Foundation/Foundation.h>
#include "AlchemyCoreMedia.h"

NS_ASSUME_NONNULL_BEGIN

/* A stream's sample format and frame durations; each frame duration in
   validFrameDurations is a CMTime dictionary (CMTimeCopyAsDictionary). */
@interface CMIOExtensionStreamFormat : NSObject
- (instancetype)initWithFormatDescription:(CMFormatDescriptionRef)formatDescription maxFrameDuration:(CMTime)maxFrameDuration
                         minFrameDuration:(CMTime)minFrameDuration
                      validFrameDurations:(nullable NSArray*)validFrameDurations NS_REFINED_FOR_SWIFT;
@property (atomic, readonly) CMFormatDescriptionRef formatDescription;
@property (atomic, readonly) CMTime maxFrameDuration;
@property (atomic, readonly) CMTime minFrameDuration;
@property (atomic, readonly, nullable) NSArray* validFrameDurations NS_REFINED_FOR_SWIFT;
@end

/* A stream's properties; frameDuration is a CMTime dictionary. */
@interface CMIOExtensionStreamProperties : NSObject
@property (atomic, strong, nullable) NSNumber* activeFormatIndex NS_REFINED_FOR_SWIFT;
@property (atomic, strong, nullable) NSDictionary* frameDuration NS_REFINED_FOR_SWIFT;
@property (atomic, strong, nullable) NSNumber* sinkBufferQueueSize NS_REFINED_FOR_SWIFT;
@property (atomic, strong, nullable) NSNumber* sinkBuffersRequiredForStartup NS_REFINED_FOR_SWIFT;
@end

/* A device's properties; transportType is an IOKit audio transport type constant. */
@interface CMIOExtensionDeviceProperties : NSObject
@property (atomic, strong, nullable) NSNumber* transportType NS_REFINED_FOR_SWIFT;
@end

/* CoreMedia's CMTime dictionary representation, declared with the toll-free
   bridged NSDictionary for CFDictionaryRef. */
NSDictionary* CMTimeCopyAsDictionary(CMTime time, const void* _Nullable allocator) NS_RETURNS_RETAINED;
CMTime CMTimeMakeFromDictionary(NSDictionary* _Nullable dictionaryRepresentation);

NS_ASSUME_NONNULL_END
#endif
