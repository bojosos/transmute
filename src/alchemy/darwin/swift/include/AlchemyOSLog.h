#ifndef ALCHEMY_QLAB_OSLOG_H
#define ALCHEMY_QLAB_OSLOG_H
/* The OSLog framework's log-store classes as the Swift ClangImporter sees
   them, for Alchemy's project-authored OSLog overlay (graphics/swift/OSLog).
   Independently authored from the published OSLog interface; no SDK
   content. The classes are declared runtime-visible so the overlay finds
   them by name through the Objective-C runtime rather than binding class
   symbols. */
#include <Foundation/Foundation.h>

#ifdef __OBJC__
NS_ASSUME_NONNULL_BEGIN

@class OSLogEnumerator;

typedef NS_OPTIONS(NSUInteger, OSLogEnumeratorOptions) {
    OSLogEnumeratorReverse = 0x01,
} NS_SWIFT_NAME(OSLogEnumerator.Options);

__attribute__((objc_runtime_visible))
@interface OSLogEntry : NSObject
@property (nonatomic, readonly) NSString* composedMessage;
@property (nonatomic, readonly) NSDate* date;
@end

__attribute__((objc_runtime_visible))
@interface OSLogPosition : NSObject
@end

__attribute__((objc_runtime_visible))
@interface OSLogEnumerator : NSEnumerator<OSLogEntry*>
@end

__attribute__((objc_runtime_visible))
@interface OSLogStore : NSObject
- (nullable OSLogEnumerator*)entriesEnumeratorWithOptions:(OSLogEnumeratorOptions)options
                                                  position:(nullable OSLogPosition*)position
                                                 predicate:(nullable NSPredicate*)predicate
                                                     error:(NSError**)error NS_REFINED_FOR_SWIFT;
- (nullable OSLogEnumerator*)entriesEnumeratorAndReturnError:(NSError**)error NS_REFINED_FOR_SWIFT;
@end

NS_ASSUME_NONNULL_END
#endif

#endif
