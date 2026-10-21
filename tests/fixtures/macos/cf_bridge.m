#include "alchemy_objc_fixture.h"
int main(void) {
    @autoreleasepool {
        NSString *literal = @"Alchemy";
        NSString *text = [NSString stringWithUTF8String:"Alchemy"];
        NSString *unicode = [[NSString alloc] initWithUTF8String:"A\xf0\x9f\x98\x80"];
        if (![literal isEqualToString:text] || CFStringGetLength((CFStringRef)unicode) != 3 ||
            [unicode characterAtIndex:1] != 0xd83d || [unicode characterAtIndex:2] != 0xde00) return 1;
        if (strcmp([text UTF8String], "Alchemy") || !CFEqual((CFTypeRef)text, (CFTypeRef)literal)) return 2;
        if (CFRetain((CFTypeRef)text) != (CFTypeRef)text) return 3;
        CFRelease((CFTypeRef)text);
        NSNumber *number = [NSNumber numberWithInt:27];
        NSNumber *real = [NSNumber numberWithDouble:13.5];
        NSNumber *boolean = [NSNumber numberWithBool:YES];
        if ([number longLongValue] != 27 || [real doubleValue] != 13.5 || ![boolean boolValue]) return 4;
        const unsigned char bytes[] = {0, 1, 255};
        NSData *data = [NSData dataWithBytes:bytes length:3];
        if (CFDataGetLength((CFDataRef)data) != 3 || ((const unsigned char *)[data bytes])[2] != 255) return 5;
        id values[] = {text, number, data};
        NSArray *array = [NSArray arrayWithObjects:values count:3];
        id keys[] = {literal}; id mapped[] = {array};
        NSDictionary *dictionary = [NSDictionary dictionaryWithObjects:mapped forKeys:keys count:1];
        if (CFArrayGetValueAtIndex((CFArrayRef)array, 1) != (CFTypeRef)number ||
            [dictionary objectForKey:text] != array || CFDictionaryGetValue((CFDictionaryRef)dictionary, (CFTypeRef)literal) != (CFTypeRef)array) return 6;
        NSString *joined = [text stringByAppendingString:@" runtime"];
        if (strcmp([joined UTF8String], "Alchemy runtime") || [joined length] != 15) return 7;
        if (NSClassFromString(@"NSString") != [NSString class] ||
            ![NSStringFromClass([NSNumber class]) isEqualToString:@"NSNumber"] ||
            ![NSStringFromSelector(NSSelectorFromString(@"length")) isEqualToString:@"length"]) return 8;
        const unichar embedded[] = {'A', 0, 'B'};
        NSString *binaryString = [[NSString alloc] initWithCharacters:embedded length:3];
        const char *binaryUTF8 = [binaryString UTF8String];
        if ([binaryString length] != 3 || !binaryUTF8 || binaryUTF8[0] != 'A' || binaryUTF8[1] != 0 || binaryUTF8[2] != 'B') return 9;
        NSArray *empty = [[NSArray alloc] init];
        if ([empty count]) return 10;
        [empty release]; [binaryString release];
        [unicode release];
    }
    puts("CoreFoundation Objective-C bridge: 27");
    return 0;
}
