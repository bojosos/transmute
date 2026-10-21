#ifndef ALCHEMY_QLAB_AVFOUNDATION_H
#define ALCHEMY_QLAB_AVFOUNDATION_H
/* The AVFoundation capture-output classes the project-authored AVFoundation
   overlay (graphics/swift/AVFoundation) extends, as the Swift ClangImporter
   sees them. Independently authored from the published AVFoundation
   interface; no SDK content. The classes are runtime-visible: the overlay
   reaches them through the Objective-C runtime and binds no AVFoundation
   symbols. */
#include <stdint.h>
#include <Foundation/Foundation.h>

typedef uint32_t FourCharCode;
typedef FourCharCode OSType;

#ifdef __OBJC__
NS_ASSUME_NONNULL_BEGIN

/* NSNumber's unsigned 32-bit accessor (Foundation). */
@interface NSNumber (AlchemyAVFoundation)
@property (readonly) unsigned int unsignedIntValue;
@end

__attribute__((objc_runtime_visible))
@interface AVCaptureOutput : NSObject
@end

__attribute__((objc_runtime_visible))
@interface AVCaptureVideoDataOutput : AVCaptureOutput
/* CVPixelBuffer format types (NSNumber-wrapped OSTypes) the output supports. */
@property (nonatomic, readonly) NSArray<NSNumber*>* availableVideoCVPixelFormatTypes NS_REFINED_FOR_SWIFT;
@end

__attribute__((objc_runtime_visible))
@interface AVCapturePhotoOutput : AVCaptureOutput
/* Pixel format types (NSNumber-wrapped OSTypes) available for uncompressed photo capture. */
@property (nonatomic, readonly) NSArray<NSNumber*>* availablePhotoPixelFormatTypes NS_REFINED_FOR_SWIFT;
/* Bayer RAW pixel format types (NSNumber-wrapped OSTypes) available for RAW capture. */
@property (nonatomic, readonly) NSArray<NSNumber*>* availableRawPhotoPixelFormatTypes NS_REFINED_FOR_SWIFT;
@end

NS_ASSUME_NONNULL_END
#endif

#endif
