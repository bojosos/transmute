/* Bonjour guest fixture (owner: v-netsec). Compiled ARM64 code drives the
 * dns_sd client API the way libndi does: a shared connection
 * (DNSServiceCreateConnection + kDNSServiceFlagsShareConnection), a
 * registration with a TXT record built by the TXTRecord helpers (twelve
 * arguments, four of them stack-packed by the compiler), a browse that finds
 * and later loses it, a resolve whose reply takes stack arguments, an
 * address lookup, and poll() + DNSServiceProcessResult on the shared socket.
 * The dns_sd entry points are looked up with dlsym (the link stubs do not
 * list them). Prototypes are project-authored from the documented API. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int getpid(void);   /* not declared by the runtime headers */

typedef struct _DNSServiceRef_t *DNSServiceRef;
typedef uint32_t DNSServiceFlags;
typedef int32_t DNSServiceErrorType;
typedef union { char PrivateData[16]; char *ForceNaturalAlignment; } TXTRecordRef;

typedef void (*RegisterReply)(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType, const char *, const char *, const char *, void *);
typedef void (*BrowseReply)(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType, const char *, const char *, const char *, void *);
typedef void (*ResolveReply)(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType, const char *, const char *, uint16_t, uint16_t,
                             const unsigned char *, void *);
typedef void (*AddressReply)(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType, const char *, const void *, uint32_t, void *);

static DNSServiceErrorType (*CreateConnection)(DNSServiceRef *);
static int (*RefSockFD)(DNSServiceRef);
static DNSServiceErrorType (*ProcessResult)(DNSServiceRef);
static void (*RefDeallocate)(DNSServiceRef);
static DNSServiceErrorType (*Register)(DNSServiceRef *, DNSServiceFlags, uint32_t, const char *, const char *, const char *, const char *,
                                      uint16_t, uint16_t, const void *, RegisterReply, void *);
static DNSServiceErrorType (*Browse)(DNSServiceRef *, DNSServiceFlags, uint32_t, const char *, const char *, BrowseReply, void *);
static DNSServiceErrorType (*Resolve)(DNSServiceRef *, DNSServiceFlags, uint32_t, const char *, const char *, const char *, ResolveReply, void *);
static DNSServiceErrorType (*GetAddrInfo)(DNSServiceRef *, DNSServiceFlags, uint32_t, uint32_t, const char *, AddressReply, void *);
static int (*ConstructFullName)(char *, const char *, const char *, const char *);
static void (*TXTCreate)(TXTRecordRef *, uint16_t, void *);
static void (*TXTDeallocate)(TXTRecordRef *);
static DNSServiceErrorType (*TXTSetValue)(TXTRecordRef *, const char *, uint8_t, const void *);
static uint16_t (*TXTGetLength)(const TXTRecordRef *);
static const void *(*TXTGetBytes)(const TXTRecordRef *);
static const void *(*TXTGetValue)(uint16_t, const void *, const char *, uint8_t *);
struct pollfd_t { int fd; short events, revents; };
static int (*poll_function)(struct pollfd_t *, unsigned int, int);

enum { flag_more = 1, flag_add = 2, flag_share = 0x4000 };
static const char *type = "_alchemyfx._tcp";
static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("Alchemy Bonjour fixture: FAIL %s\n", what); failures++; }
}

struct State {
    char name[64];
    int registered, found, lost, resolved, addresses;
    DNSServiceErrorType error;
    uint16_t port;
    char host[256];
    char kind[16];
};

static void on_register(DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType error, const char *name, const char *regtype,
                        const char *domain, void *context) {
    struct State *state = context;
    (void)ref; (void)regtype;
    if (error) { state->error = error; return; }
    check((flags & flag_add) != 0 && strcmp(name, state->name) == 0 && strcmp(domain, "local.") == 0, "register reply");
    state->registered = 1;
}
static void on_browse(DNSServiceRef ref, DNSServiceFlags flags, uint32_t index, DNSServiceErrorType error, const char *name,
                      const char *regtype, const char *domain, void *context) {
    struct State *state = context;
    (void)ref; (void)index; (void)domain;
    if (error) { state->error = error; return; }
    if (strcmp(name, state->name) != 0) return;
    check(strcmp(regtype, "_alchemyfx._tcp.") == 0, "browse reply type");
    if (flags & flag_add) state->found = 1;
    else state->lost = 1;
}
static void on_resolve(DNSServiceRef ref, DNSServiceFlags flags, uint32_t index, DNSServiceErrorType error, const char *full,
                       const char *host, uint16_t port, uint16_t length, const unsigned char *txt, void *context) {
    struct State *state = context;
    char expected[1009];
    uint8_t size = 0;
    const char *kind;
    (void)ref; (void)flags; (void)index;
    if (error) { state->error = error; return; }
    ConstructFullName(expected, state->name, type, "local.");
    check(strcmp(full, expected) == 0, "resolve full name");
    state->port = (uint16_t)((port >> 8) | (port << 8));
    snprintf(state->host, sizeof state->host, "%s", host);
    kind = TXTGetValue(length, txt, "kind", &size);
    if (kind && size < sizeof state->kind) { memcpy(state->kind, kind, size); state->kind[size] = 0; }
    state->resolved = 1;
}
static void on_address(DNSServiceRef ref, DNSServiceFlags flags, uint32_t index, DNSServiceErrorType error, const char *host,
                       const void *address, uint32_t ttl, void *context) {
    struct State *state = context;
    const unsigned char *bytes = address;
    (void)ref; (void)index; (void)host; (void)ttl;
    if (error) { state->error = error; return; }
    check((flags & flag_add) && (bytes[1] == 2 || bytes[1] == 30), "address reply sockaddr");
    state->addresses++;
}

/* Runs DNSServiceProcessResult while the connection is readable, up to 20 s. */
static int pump(DNSServiceRef connection, int *flag) {
    struct pollfd_t entry;
    for (int round = 0; round < 200 && !*flag; ++round) {
        entry.fd = RefSockFD(connection);
        entry.events = 1;
        entry.revents = 0;
        if (poll_function(&entry, 1, 100) == 1 && ProcessResult(connection) != 0) return 0;
    }
    return *flag;
}

#define LOOKUP(target, name) do { *(void **)&(target) = dlsym(RTLD_DEFAULT, name); check((target) != 0, name); } while (0)

int main(void) {
    printf("Alchemy Bonjour fixture: launched\n");
    LOOKUP(CreateConnection, "DNSServiceCreateConnection");
    LOOKUP(RefSockFD, "DNSServiceRefSockFD");
    LOOKUP(ProcessResult, "DNSServiceProcessResult");
    LOOKUP(RefDeallocate, "DNSServiceRefDeallocate");
    LOOKUP(Register, "DNSServiceRegister");
    LOOKUP(Browse, "DNSServiceBrowse");
    LOOKUP(Resolve, "DNSServiceResolve");
    LOOKUP(GetAddrInfo, "DNSServiceGetAddrInfo");
    LOOKUP(ConstructFullName, "DNSServiceConstructFullName");
    LOOKUP(TXTCreate, "TXTRecordCreate");
    LOOKUP(TXTDeallocate, "TXTRecordDeallocate");
    LOOKUP(TXTSetValue, "TXTRecordSetValue");
    LOOKUP(TXTGetLength, "TXTRecordGetLength");
    LOOKUP(TXTGetBytes, "TXTRecordGetBytesPtr");
    LOOKUP(TXTGetValue, "TXTRecordGetValuePtr");
    LOOKUP(poll_function, "poll");
    if (failures) return 1;

    struct State state;
    memset(&state, 0, sizeof state);
    snprintf(state.name, sizeof state.name, "Alchemy Fixture %d", (int)getpid());

    DNSServiceRef connection = 0;
    DNSServiceErrorType status = CreateConnection(&connection);
    if (status == -65563) {   /* kDNSServiceErr_ServiceNotRunning: no Windows DNS-SD client */
        printf("Alchemy Bonjour fixture: DNS-SD unavailable, skipped\nAlchemy Bonjour fixture: complete\n");
        return 0;
    }
    check(status == 0 && connection != 0, "DNSServiceCreateConnection");
    if (getenv("ALCHEMY_TEST_NO_MDNS") && *getenv("ALCHEMY_TEST_NO_MDNS")) {
        RefDeallocate(connection);
        printf("Alchemy Bonjour fixture: ALCHEMY_TEST_NO_MDNS set, discovery skipped\nAlchemy Bonjour fixture: complete\n");
        return failures ? 1 : 0;
    }

    TXTRecordRef txt;
    TXTCreate(&txt, 0, 0);
    check(TXTSetValue(&txt, "txtvers", 1, "1") == 0 && TXTSetValue(&txt, "kind", 7, "fixture") == 0, "TXTRecordSetValue");

    DNSServiceRef browse = connection, registration = connection, resolve = connection, lookup = connection;
    check(Browse(&browse, flag_share, 0, type, "local.", on_browse, &state) == 0, "DNSServiceBrowse");
    /* port 47125 in network byte order; txtLen, txtRecord, callBack and context go on the stack. */
    check(Register(&registration, flag_share, 0, state.name, type, 0, 0, (uint16_t)((47125 >> 8) | (47125 << 8)), TXTGetLength(&txt),
                   TXTGetBytes(&txt), on_register, &state) == 0, "DNSServiceRegister");
    TXTDeallocate(&txt);
    check(pump(connection, &state.registered), "registration");
    check(pump(connection, &state.found), "browse finds the registration");
    check(Resolve(&resolve, flag_share, 0, state.name, type, "local.", on_resolve, &state) == 0, "DNSServiceResolve");
    check(pump(connection, &state.resolved), "resolve");
    check(state.port == 47125 && strcmp(state.kind, "fixture") == 0 && state.host[0], "resolved port, TXT and host");
    RefDeallocate(resolve);
    if (state.host[0]) {
        check(GetAddrInfo(&lookup, flag_share, 0, 0, state.host, on_address, &state) == 0, "DNSServiceGetAddrInfo");
        check(pump(connection, &state.addresses), "address lookup");
        RefDeallocate(lookup);
    }
    RefDeallocate(registration);
    check(pump(connection, &state.lost), "browse reports the withdrawal");
    RefDeallocate(connection);   /* ends the browse too */
    if (state.error) printf("Alchemy Bonjour fixture: reply error %d\n", (int)state.error);
    printf("Alchemy Bonjour fixture: %s %s:%u\n", state.name, state.host, state.port);
    if (failures) return 1;
    printf("Alchemy Bonjour fixture: complete\n");
    return 0;
}
