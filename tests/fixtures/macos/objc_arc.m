#include "alchemy_objc_fixture.h"
static int destroyed;
@interface ArcPayload : NSObject
@end
@implementation ArcPayload
- (void)dealloc { ++destroyed; }
@end
@interface ArcBox : NSObject { id _payload; }
- (id)initWithPayload:(id)payload;
- (id)payload;
@end
@implementation ArcBox
- (id)initWithPayload:(id)payload { _payload = payload; return self; }
- (id)payload { return _payload; }
@end
static int exercise(void) {
    @autoreleasepool {
        ArcPayload *payload = [[ArcPayload alloc] init];
        ArcBox *box = [[ArcBox alloc] initWithPayload:payload];
        if ([box payload] != payload || destroyed != 0) return 1;
        payload = nil;
        if (destroyed != 0) return 2;
        box = nil;
    }
    return destroyed == 1 ? 0 : 3;
}
int main(void) {
    int result = exercise();
    if (result) return result;
    puts("Objective-C ARC: 1");
    return 0;
}
