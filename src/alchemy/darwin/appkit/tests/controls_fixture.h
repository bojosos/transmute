#ifndef ALCHEMY_CONTROLS_FIXTURE_H
#define ALCHEMY_CONTROLS_FIXTURE_H
/* Project-authored declarations for the AppKit controls guest fixture (owner:
 * AppKit): the controls and HIToolbox calls a wxOSX program makes, beyond
 * runtime/include/AppKit/AppKit.h. Selectors and layouts follow the public
 * interface; no Apple header text is used. */
#include <AppKit/AppKit.h>
#include <CoreFoundation/CoreFoundation.h>

@interface NSControl : NSView
- (double)doubleValue;
- (void)setDoubleValue:(double)value;
- (NSString *)stringValue;
- (void)setStringValue:(NSString *)value;
- (void)setTarget:(id)target;
- (void)setAction:(SEL)action;
- (BOOL)sendAction:(SEL)action to:(id)target;
@end
@interface NSSlider : NSControl
- (double)minValue;
- (void)setMinValue:(double)value;
- (double)maxValue;
- (void)setMaxValue:(double)value;
- (void)setNumberOfTickMarks:(NSInteger)count;
- (double)closestTickMarkValueToValue:(double)value;
- (BOOL)isVertical;
@end
@interface NSStepper : NSControl
- (void)setMaxValue:(double)value;
- (void)setIncrement:(double)value;
- (BOOL)valueWraps;
@end
@interface NSTextField : NSControl
@end
@interface NSComboBox : NSTextField
- (void)addItemWithObjectValue:(id)value;
- (NSInteger)numberOfItems;
- (void)selectItemAtIndex:(NSInteger)index;
- (NSInteger)indexOfSelectedItem;
- (void)setDelegate:(id)delegate;
@end
@interface NSPrintInfo : NSObject
+ (NSPrintInfo *)sharedPrintInfo;
- (NSSize)paperSize;
- (void)setOrientation:(NSInteger)orientation;
@end
@interface NSPasteboardItem : NSObject
- (BOOL)setString:(NSString *)string forType:(NSString *)type;
- (NSString *)stringForType:(NSString *)type;
@end

/* HIShape (Carbon / ApplicationServices). */
typedef const struct __HIShape *HIShapeRef;
typedef struct __HIShape *HIMutableShapeRef;
typedef int OSStatus;
HIShapeRef HIShapeCreateWithRect(const CGRect *rect);
HIMutableShapeRef HIShapeCreateMutableCopy(HIShapeRef shape);
HIShapeRef HIShapeCreateIntersection(HIShapeRef a, HIShapeRef b);
OSStatus HIShapeUnionWithRect(HIMutableShapeRef shape, const CGRect *rect);
OSStatus HIShapeOffset(HIMutableShapeRef shape, CGFloat dx, CGFloat dy);
OSStatus HIShapeDifference(HIShapeRef a, HIShapeRef b, HIMutableShapeRef result);
CGRect *HIShapeGetBounds(HIShapeRef shape, CGRect *bounds);
unsigned char HIShapeContainsPoint(HIShapeRef shape, const CGPoint *point);
unsigned char HIShapeIsRectangular(HIShapeRef shape);
unsigned char HIShapeIsEmpty(HIShapeRef shape);

#endif
