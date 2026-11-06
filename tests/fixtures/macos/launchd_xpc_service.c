/* An XPC service bundled in the launchd_model test application
 * (LaunchdModel.app/Contents/XPCServices/com.alchemy.test.echo.xpc). Project-authored, no
 * SDK headers. xpc_main serves each peer: a request {value} is answered with value + 1, the
 * service's pid and ppid, its XPC_SERVICE_NAME and the variable its Info.plist sets. */
typedef unsigned long size_t;
typedef int pid_t;
typedef void* xpc_object_t;

extern pid_t getpid(void);
extern pid_t getppid(void);
extern char* getenv(const char*);
extern void xpc_main(void*);
extern void xpc_connection_set_event_handler(xpc_object_t, void*);
extern void xpc_connection_resume(xpc_object_t);
extern void xpc_connection_send_message(xpc_object_t, xpc_object_t);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern xpc_object_t xpc_dictionary_get_remote_connection(xpc_object_t);
extern void xpc_dictionary_set_string(xpc_object_t, const char*, const char*);
extern void xpc_dictionary_set_int64(xpc_object_t, const char*, long long);
extern long long xpc_dictionary_get_int64(xpc_object_t, const char*);
extern const void* xpc_get_type(xpc_object_t);
extern void xpc_release(xpc_object_t);
extern const char _xpc_type_dictionary, _xpc_type_connection;
extern void* _NSConcreteGlobalBlock[];

struct block_descriptor { unsigned long reserved, size; };
struct block { void* isa; int flags; int reserved; void* invoke; struct block_descriptor* descriptor; };
static struct block_descriptor block_size = {0, sizeof(struct block)};
#define GLOBAL_BLOCK(function) { _NSConcreteGlobalBlock, 1 << 28, 0, (void*)(function), &block_size }

static void message(struct block* self, xpc_object_t event) {
    (void)self;
    if (xpc_get_type(event) != &_xpc_type_dictionary) return;
    xpc_object_t reply = xpc_dictionary_create_reply(event);
    if (!reply) return;
    const char* service = getenv("XPC_SERVICE_NAME");
    const char* extra = getenv("ALCHEMY_TEST_XPC_ENV");
    xpc_dictionary_set_int64(reply, "value", xpc_dictionary_get_int64(event, "value") + 1);
    xpc_dictionary_set_int64(reply, "pid", getpid());
    xpc_dictionary_set_int64(reply, "ppid", getppid());
    xpc_dictionary_set_string(reply, "service", service ? service : "");
    xpc_dictionary_set_string(reply, "extra", extra ? extra : "");
    xpc_connection_send_message(xpc_dictionary_get_remote_connection(event), reply);
    xpc_release(reply);
}
static struct block message_block = GLOBAL_BLOCK(message);

static void peer(struct block* self, xpc_object_t connection) {
    (void)self;
    if (xpc_get_type(connection) != &_xpc_type_connection) return;
    xpc_connection_set_event_handler(connection, &message_block);
    xpc_connection_resume(connection);
}
static struct block peer_block = GLOBAL_BLOCK(peer);

int main(void) {
    xpc_main(&peer_block);
    return 1;   /* xpc_main does not return */
}
