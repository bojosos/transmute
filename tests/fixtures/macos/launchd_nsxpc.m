#include <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* NSXPCConnection end to end (darwin/launchd/nsxpc.cpp over the C XPC connections): the
 * client (NsxpcModel.app/Contents/MacOS/nsxpc_client) reaches the application's bundled
 * service com.alchemy.test.nsxpc (NSXPC_SERVICE: XPCServices/com.alchemy.test.nsxpc.xpc),
 * which serves an AlchemyCalc object through +[NSXPCListener serviceListener], as Sparkle's
 * Downloader.xpc does. Covered: integer, floating-point, BOOL and object arguments; reply
 * blocks with arguments of each kind; a captured (stack) reply block; one-way messages in
 * order; the synchronous proxy; the peer's process identifier; +currentConnection in the
 * service; -invalidate's invalidation handler; and the error handler of a proxy for a
 * service that does not exist. Project-authored; classes are looked up by name. Prints
 * "ok nsxpc" when every check passes (the process model driver's convention). */

@class Protocol;
@interface NSObject (NsxpcFixture)
+ (id)interfaceWithProtocol:(Protocol *)protocol;
+ (id)serviceListener;
+ (id)currentConnection;
- (id)initWithServiceName:(id)name;
- (void)setRemoteObjectInterface:(id)interface;
- (void)setExportedInterface:(id)interface;
- (void)setExportedObject:(id)object;
- (void)setInvalidationHandler:(void (^)(void))handler;
- (void)setDelegate:(id)delegate;
- (id)remoteObjectProxyWithErrorHandler:(void (^)(id error))handler;
- (id)synchronousRemoteObjectProxyWithErrorHandler:(void (^)(id error))handler;
- (void)resume;
- (void)invalidate;
- (int)processIdentifier;
- (NSInteger)code;
- (id)domain;
@end

extern int getpid(void);

@protocol AlchemyCalc
- (void)add:(NSInteger)a to:(NSInteger)b reply:(void (^)(NSInteger sum, NSString *who))reply;
- (void)scale:(double)value by:(float)factor flag:(BOOL)flag reply:(void (^)(double result, BOOL flag))reply;
- (void)echo:(NSDictionary *)dictionary reply:(void (^)(NSDictionary *echoed, int pid))reply;
- (void)note:(NSString *)text;
- (void)lastNote:(void (^)(NSString *text))reply;
@end

static id S(const char *text) { return [NSString stringWithUTF8String:text]; }
static int same(id value, const char *text) { return value && strcmp([value UTF8String], text) == 0; }

#ifdef NSXPC_SERVICE

@interface Calc : NSObject <AlchemyCalc> { id last; }
@end
@implementation Calc
- (void)add:(NSInteger)a to:(NSInteger)b reply:(void (^)(NSInteger, NSString *))reply {
    const char *service = getenv("XPC_SERVICE_NAME");
    char who[256];
    snprintf(who, sizeof who, "%s|%s", service ? service : "", [NSClassFromString(@"NSXPCConnection") currentConnection] ? "current" : "none");
    reply(a + b, S(who));
}
- (void)scale:(double)value by:(float)factor flag:(BOOL)flag reply:(void (^)(double, BOOL))reply {
    reply(value * factor, !flag);
}
- (void)echo:(NSDictionary *)dictionary reply:(void (^)(NSDictionary *, int))reply {
    reply(dictionary, getpid());
}
- (void)note:(NSString *)text { [last release]; last = [text retain]; }
- (void)lastNote:(void (^)(NSString *))reply { reply(last); }
@end

@interface Delegate : NSObject
@end
@implementation Delegate
- (BOOL)listener:(id)listener shouldAcceptNewConnection:(id)connection {
    (void)listener;
    [connection setExportedInterface:[NSClassFromString(@"NSXPCInterface") interfaceWithProtocol:@protocol(AlchemyCalc)]];
    [connection setExportedObject:[[Calc new] autorelease]];
    [connection resume];
    return YES;
}
@end

int main(void) {
    id listener = [NSClassFromString(@"NSXPCListener") serviceListener];
    [listener setDelegate:[Delegate new]];
    [listener resume];   /* serves until the process ends */
    return 1;
}

#else

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL nsxpc: %s\n", what); fflush(stdout); failures++; }
}
static dispatch_semaphore_t done;
static int wait_done(void) { return dispatch_semaphore_wait(done, dispatch_time(0, 30LL * 1000000000LL)) == 0; }

static NSInteger sum_result;
static id who_result;
static double scale_result;
static BOOL flag_result;
static id echo_result;
static int echo_pid;
static id note_result;
static NSInteger error_code;
static id error_domain;

static id connect_to(const char *name) {
    id connection = [[NSClassFromString(@"NSXPCConnection") alloc] initWithServiceName:S(name)];
    [connection setRemoteObjectInterface:[NSClassFromString(@"NSXPCInterface") interfaceWithProtocol:@protocol(AlchemyCalc)]];
    [connection resume];
    return connection;
}

static void error_handler(id error) {
    error_code = [error code];
    error_domain = [[error domain] retain];
    dispatch_semaphore_signal(done);
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "nsxpc") != 0) { printf("FAIL nsxpc: usage: nsxpc_client nsxpc\n"); return 2; }
    done = dispatch_semaphore_create(0);
    id connection = connect_to("com.alchemy.test.nsxpc");
    id<AlchemyCalc> proxy = [connection remoteObjectProxyWithErrorHandler:^(id error) { error_handler(error); }];

    /* Integer arguments, a reply with an integer and an object; +currentConnection in the service. */
    [proxy add:2 to:40 reply:^(NSInteger sum, NSString *who) {
        sum_result = sum; who_result = [who retain]; dispatch_semaphore_signal(done);
    }];
    check(wait_done(), "add: reply");
    check(sum_result == 42, "add: sum");
    check(same(who_result, "com.alchemy.test.nsxpc|current"), "add: service name and currentConnection");

    /* Floating-point and BOOL arguments and replies. */
    [proxy scale:1.5 by:4.0f flag:YES reply:^(double result, BOOL flag) {
        scale_result = result; flag_result = flag; dispatch_semaphore_signal(done);
    }];
    check(wait_done(), "scale: reply");
    check(scale_result == 6.0 && flag_result == NO, "scale: double, float and BOOL");

    /* An object argument through the archive; a captured (stack) reply block. */
    id keys[] = {S("name"), S("count")};
    id values[] = {S("cue"), [NSNumber numberWithInt:7]};
    id dictionary = [NSDictionary dictionaryWithObjects:values forKeys:keys count:2];
    int captured = 0;
    int *slot = &captured;
    [proxy echo:dictionary reply:^(NSDictionary *echoed, int pid) {
        echo_result = [echoed retain]; echo_pid = pid; *slot = 1; dispatch_semaphore_signal(done);
    }];
    check(wait_done(), "echo: reply");
    check(captured == 1, "echo: captured block variable");
    check(echo_result && same([echo_result objectForKey:S("name")], "cue") && [[echo_result objectForKey:S("count")] intValue] == 7,
          "echo: dictionary round trip");
    check(echo_pid > 0 && echo_pid != getpid(), "echo: answered from the service process");
    check([connection processIdentifier] == echo_pid, "processIdentifier is the service's");

    /* One-way messages arrive before a later message's reply. */
    [proxy note:S("first")];
    [proxy note:S("second")];
    [proxy lastNote:^(NSString *text) { note_result = [text retain]; dispatch_semaphore_signal(done); }];
    check(wait_done(), "lastNote: reply");
    check(same(note_result, "second"), "one-way messages in order");

    /* The synchronous proxy runs the reply before the call returns. */
    sum_result = 0;
    [[connection synchronousRemoteObjectProxyWithErrorHandler:^(id error) { error_handler(error); }] add:5 to:6 reply:^(NSInteger sum, NSString *who) {
        (void)who; sum_result = sum;
    }];
    check(sum_result == 11, "synchronous proxy");

    /* -invalidate runs the invalidation handler. */
    static int invalidated;
    [connection setInvalidationHandler:^{ invalidated = 1; dispatch_semaphore_signal(done); }];
    [connection invalidate];
    check(wait_done() && invalidated, "invalidation handler");

    /* A service that does not exist: the proxy's error handler, NSCocoaErrorDomain 4099. */
    id missing = connect_to("com.alchemy.test.missing");
    [[missing remoteObjectProxyWithErrorHandler:^(id error) { error_handler(error); }] add:1 to:1 reply:^(NSInteger sum, NSString *who) {
        (void)sum; (void)who; check(0, "missing service: reply");
    }];
    check(wait_done(), "missing service: error handler");
    check(error_code == 4099 && same(error_domain, "NSCocoaErrorDomain"), "missing service: NSXPCConnectionInvalid");
    [missing invalidate];

    if (failures) return 1;
    printf("ok nsxpc\n");
    return 0;
}

#endif
