#include <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
/* Keyed archive round trips under alchemy-fx (owner: QLab group G1,
 * kit/completion/keyed_archive.cpp): archives written by Alchemy's
 * NSKeyedArchiver and read back by its NSKeyedUnarchiver. Arrays,
 * dictionaries, strings, numbers, data, nested collections, a guest
 * NSCoding / NSSecureCoding class, the secure-coding allow lists and the
 * legacy (non-secure) factories. The classes are looked up by name, so the
 * fixture links only the Foundation identity. */

@interface NSObject (ArchiveFixture)
+ (id)archivedDataWithRootObject:(id)root;
+ (id)archivedDataWithRootObject:(id)root requiringSecureCoding:(BOOL)secure error:(id *)error;
+ (id)unarchiveObjectWithData:(id)data;
+ (id)unarchivedObjectOfClass:(Class)cls fromData:(id)data error:(id *)error;
+ (id)unarchivedObjectOfClasses:(id)classes fromData:(id)data error:(id *)error;
+ (id)setWithObjects:(id)first, ...;
- (id)initRequiringSecureCoding:(BOOL)secure;
- (id)initForReadingFromData:(id)data error:(id *)error;
- (id)initForReadingWithData:(id)data;
- (void)setRequiresSecureCoding:(BOOL)secure;
- (void)encodeObject:(id)object forKey:(id)key;
- (void)encodeInteger:(NSInteger)value forKey:(id)key;
- (void)encodeDouble:(double)value forKey:(id)key;
- (void)encodeBool:(BOOL)value forKey:(id)key;
- (void)finishEncoding;
- (id)encodedData;
- (id)decodeObjectForKey:(id)key;
- (id)decodeObjectOfClass:(Class)cls forKey:(id)key;
- (id)decodeObjectOfClasses:(id)classes forKey:(id)key;
- (NSInteger)decodeIntegerForKey:(id)key;
- (double)decodeDoubleForKey:(id)key;
- (BOOL)decodeBoolForKey:(id)key;
- (BOOL)containsValueForKey:(id)key;
- (void)finishDecoding;
- (NSInteger)code;
- (id)domain;
@end

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("Alchemy keyed archive fixture: FAIL %s\n", what); failures++; }
}
static int same_string(id value, const char *text) {
    return value && [value isKindOfClass:NSClassFromString(@"NSString")] && strcmp([value UTF8String], text) == 0;
}

@interface Cue : NSObject {
@public
    id name;
    NSInteger number;
    double duration;
    BOOL armed;
    id children;
}
@end
@implementation Cue
+ (BOOL)supportsSecureCoding { return YES; }
- (void)encodeWithCoder:(id)coder {
    [coder encodeObject:name forKey:@"name"];
    [coder encodeInteger:number forKey:@"number"];
    [coder encodeDouble:duration forKey:@"duration"];
    [coder encodeBool:armed forKey:@"armed"];
    [coder encodeObject:children forKey:@"$children"];   /* a '$' key is escaped */
}
- (id)initWithCoder:(id)coder {
    self = [super init];
    if (!self) return nil;
    name = [[coder decodeObjectOfClass:NSClassFromString(@"NSString") forKey:@"name"] retain];
    number = [coder decodeIntegerForKey:@"number"];
    duration = [coder decodeDoubleForKey:@"duration"];
    armed = [coder decodeBoolForKey:@"armed"];
    children = [[coder decodeObjectForKey:@"$children"] retain];
    return self;
}
@end

@interface Opaque : NSObject
@end
@implementation Opaque
+ (BOOL)supportsSecureCoding { return NO; }
- (void)encodeWithCoder:(id)coder { [coder encodeInteger:7 forKey:@"seven"]; }
- (id)initWithCoder:(id)coder { return [super init]; }
@end

int main(void) {
    id pool = [[NSClassFromString(@"NSAutoreleasePool") alloc] init];
    Class archiver = NSClassFromString(@"NSKeyedArchiver");
    Class unarchiver = NSClassFromString(@"NSKeyedUnarchiver");
    check(archiver != nil && unarchiver != nil, "archiver classes");
    puts("Alchemy keyed archive fixture: launched");

    /* An array holding one string, through the legacy factories. */
    id one = [NSArray arrayWithObject:@"QLab"];
    id data = [archiver archivedDataWithRootObject:one];
    check(data != nil && [data length] > 8 && memcmp([data bytes], "bplist00", 8) == 0, "archive is a binary property list");
    id back = [unarchiver unarchiveObjectWithData:data];
    check(back != nil && [back isKindOfClass:[NSArray class]] && [back count] == 1, "array round trip");
    check(back && same_string([back objectAtIndex:0], "QLab"), "string element");

    /* Dictionaries with numbers, booleans and nested collections. */
    id keys[4] = {@"int", @"real", @"flag", @"list"};
    id inner[2] = {@"a", [NSNumber numberWithInt:2]};
    id values[4] = {[NSNumber numberWithLongLong:-42], [NSNumber numberWithDouble:2.5], [NSNumber numberWithBool:YES],
                    [NSArray arrayWithObjects:inner count:2]};
    id dictionary = [NSDictionary dictionaryWithObjects:values forKeys:keys count:4];
    id decoded = [unarchiver unarchiveObjectWithData:[archiver archivedDataWithRootObject:dictionary]];
    check(decoded && [decoded count] == 4, "dictionary round trip");
    check(decoded && [[decoded objectForKey:@"int"] longLongValue] == -42, "integer value");
    check(decoded && [[decoded objectForKey:@"real"] doubleValue] == 2.5, "real value");
    check(decoded && [[decoded objectForKey:@"flag"] boolValue], "boolean value");
    id list = decoded ? [decoded objectForKey:@"list"] : nil;
    check(list && [list count] == 2 && same_string([list objectAtIndex:0], "a") && [[list objectAtIndex:1] intValue] == 2, "nested array");
    id bytes = [NSData dataWithBytes:"\x01\x02\x03" length:3];
    id data_back = [unarchiver unarchiveObjectWithData:[archiver archivedDataWithRootObject:bytes]];
    check(data_back && [data_back length] == 3 && memcmp([data_back bytes], "\x01\x02\x03", 3) == 0, "data round trip");
    id text_back = [unarchiver unarchiveObjectWithData:[archiver archivedDataWithRootObject:@"plain"]];
    check(same_string(text_back, "plain"), "string root");
    id number_back = [unarchiver unarchiveObjectWithData:[archiver archivedDataWithRootObject:[NSNumber numberWithInt:5]]];
    check(number_back && [number_back intValue] == 5, "number root");

    /* A guest NSSecureCoding class, shared references and secure decoding. */
    Cue *cue = [[Cue alloc] init];
    cue->name = @"Go";
    cue->number = 12;
    cue->duration = 1.25;
    cue->armed = YES;
    Cue *child = [[Cue alloc] init];
    child->name = @"Fade";
    id pair[2] = {child, child};
    cue->children = [NSArray arrayWithObjects:pair count:2];
    id error = nil;
    id secure = [archiver archivedDataWithRootObject:cue requiringSecureCoding:YES error:&error];
    check(secure != nil && error == nil, "secure archive of a guest class");
    id allowed = [NSClassFromString(@"NSSet") setWithObjects:[Cue class], [NSArray class], [NSString class], nil];
    Cue *cue_back = [unarchiver unarchivedObjectOfClasses:allowed fromData:secure error:&error];
    check(cue_back != nil && [cue_back isKindOfClass:[Cue class]], "secure unarchive of a guest class");
    if (cue_back) {
        check(same_string(cue_back->name, "Go") && cue_back->number == 12 && cue_back->duration == 1.25 && cue_back->armed, "guest class fields");
        check(cue_back->children && [cue_back->children count] == 2, "guest class children");
        if (cue_back->children && [cue_back->children count] == 2)
            check([cue_back->children objectAtIndex:0] == [cue_back->children objectAtIndex:1], "shared references decode to one object");
    }
    error = nil;
    id refused = [unarchiver unarchivedObjectOfClass:[NSArray class] fromData:secure error:&error];
    check(refused == nil && error != nil && [error code] == 4864, "a class outside the allow list is refused with 4864");
    id legacy = [unarchiver unarchiveObjectWithData:secure];
    check(legacy && [legacy isKindOfClass:[Cue class]], "legacy unarchiving reads a secure archive");

    /* Secure archiving refuses a class without NSSecureCoding; the legacy archiver accepts it. */
    error = nil;
    id opaque = [[Opaque alloc] init];
    id refused_data = [archiver archivedDataWithRootObject:opaque requiringSecureCoding:YES error:&error];
    check(refused_data == nil && error != nil && [error code] == 4866, "secure archiving refuses a non-secure class (4866)");
    id open_data = [archiver archivedDataWithRootObject:opaque];
    check(open_data != nil && [[unarchiver unarchiveObjectWithData:open_data] isKindOfClass:[Opaque class]], "legacy archiving of a non-secure class");

    /* An archiver and unarchiver used directly, with scalar keys at the top level. */
    id writer = [[archiver alloc] initRequiringSecureCoding:NO];
    [writer encodeInteger:99 forKey:@"count"];
    [writer encodeObject:@"top" forKey:@"label"];
    [writer finishEncoding];
    id reader = [[unarchiver alloc] initForReadingFromData:[writer encodedData] error:&error];
    check(reader != nil, "unarchiver from encodedData");
    if (reader) {
        [reader setRequiresSecureCoding:NO];
        check([reader decodeIntegerForKey:@"count"] == 99 && same_string([reader decodeObjectForKey:@"label"], "top"), "top-level keys");
        check(![reader containsValueForKey:@"missing"] && [reader decodeObjectForKey:@"missing"] == nil, "missing keys");
        [reader finishDecoding];
    }
    error = nil;
    id garbage = [unarchiver unarchivedObjectOfClass:[NSArray class] fromData:[NSData dataWithBytes:"nope" length:4] error:&error];
    check(garbage == nil && error != nil && [error code] == 4864, "data that is not an archive gives 4864");

    /* An empty mutable array decoded with an allow list of NSArray and a guest class. */
    id empty = [archiver archivedDataWithRootObject:[NSClassFromString(@"NSMutableArray") array] requiringSecureCoding:YES error:&error];
    id allow_list = [NSClassFromString(@"NSSet") setWithObjects:[NSArray class], [Cue class], nil];
    error = nil;
    id empty_back = [unarchiver unarchivedObjectOfClasses:allow_list fromData:empty error:&error];
    check(empty_back != nil && error == nil && [empty_back count] == 0 && [empty_back isKindOfClass:NSClassFromString(@"NSMutableArray")], "empty mutable array with an allow list");

    /* Malformed archives give nil and NSCoderReadCorruptError (4864), never a stop. */
    static const char *malformed[] = {
        /* an object without $class */
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>$archiver</key><string>NSKeyedArchiver</string>"
        "<key>$version</key><integer>100000</integer><key>$top</key><dict><key>root</key><dict><key>CF$UID</key><integer>1</integer></dict></dict>"
        "<key>$objects</key><array><string>$null</string><dict><key>NS.objects</key><array/></dict></array></dict></plist>",
        /* a class nobody registered */
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>$archiver</key><string>NSKeyedArchiver</string>"
        "<key>$version</key><integer>100000</integer><key>$top</key><dict><key>root</key><dict><key>CF$UID</key><integer>1</integer></dict></dict>"
        "<key>$objects</key><array><string>$null</string><dict><key>$class</key><dict><key>CF$UID</key><integer>2</integer></dict></dict>"
        "<dict><key>$classname</key><string>NoSuchClass</string><key>$classes</key><array><string>NoSuchClass</string></array></dict></array></dict></plist>",
        /* a reference past the object table */
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>$archiver</key><string>NSKeyedArchiver</string>"
        "<key>$version</key><integer>100000</integer><key>$top</key><dict><key>root</key><dict><key>CF$UID</key><integer>9</integer></dict></dict>"
        "<key>$objects</key><array><string>$null</string></array></dict></plist>",
        /* a class without NSSecureCoding read securely */
        0,
    };
    for (int i = 0; malformed[i]; ++i) {
        error = nil;
        id bad = [NSData dataWithBytes:malformed[i] length:strlen(malformed[i])];
        id result = [unarchiver unarchivedObjectOfClasses:allow_list fromData:bad error:&error];
        check(result == nil && error != nil && [error code] == 4864, i == 0 ? "object without $class" : i == 1 ? "unknown class" : "reference out of range");
    }
    error = nil;
    id opaque_allowed = [NSClassFromString(@"NSSet") setWithObjects:[Opaque class], nil];
    id insecure = [unarchiver unarchivedObjectOfClasses:opaque_allowed fromData:open_data error:&error];
    check(insecure == nil && error != nil && [error code] == 4864, "secure decoding refuses a class without NSSecureCoding");

    [pool drain];
    if (failures) { printf("Alchemy keyed archive fixture: %d failure(s)\n", failures); return 1; }
    puts("Alchemy keyed archive fixture: complete");
    return 0;
}
