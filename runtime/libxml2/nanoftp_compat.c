/* ABI compatibility for libxml2's nanoFTP client, which upstream removed in 2.15 but macOS's
 * libxml2.2 still exports (darktable binds five of these names). Project-authored (MIT),
 * compiled into the guest libxml2 runtime by scripts/build_alchemy_libxml2.ps1.
 *
 * FTP is not supported: every call fails the documented way, so a caller falls back or
 * reports the error. No connection is ever attempted. */
#include <stddef.h>

typedef void (*ftpListCallback)(void* userData, const char* filename, const char* attrib, const char* owner,
                                const char* group, unsigned long size, int links, int year, const char* month,
                                int day, int hour, int minute);
typedef void (*ftpDataCallback)(void* userData, const char* data, int len);

void xmlNanoFTPInit(void) {}
void xmlNanoFTPCleanup(void) {}
void xmlNanoFTPProxy(const char* host, int port, const char* user, const char* passwd, int type) {
    (void)host; (void)port; (void)user; (void)passwd; (void)type;
}
int xmlNanoFTPUpdateURL(void* ctx, const char* URL) { (void)ctx; (void)URL; return -1; }
void xmlNanoFTPScanProxy(const char* URL) { (void)URL; }
void* xmlNanoFTPNewCtxt(const char* URL) { (void)URL; return NULL; }
void xmlNanoFTPFreeCtxt(void* ctx) { (void)ctx; }
void* xmlNanoFTPConnectTo(const char* server, int port) { (void)server; (void)port; return NULL; }
void* xmlNanoFTPOpen(const char* URL) { (void)URL; return NULL; }
int xmlNanoFTPConnect(void* ctx) { (void)ctx; return -1; }
int xmlNanoFTPClose(void* ctx) { (void)ctx; return -1; }
int xmlNanoFTPQuit(void* ctx) { (void)ctx; return -1; }
int xmlNanoFTPGetResponse(void* ctx) { (void)ctx; return -1; }
int xmlNanoFTPCheckResponse(void* ctx) { (void)ctx; return -1; }
int xmlNanoFTPCwd(void* ctx, const char* directory) { (void)ctx; (void)directory; return -1; }
int xmlNanoFTPDele(void* ctx, const char* file) { (void)ctx; (void)file; return -1; }
int xmlNanoFTPGetConnection(void* ctx) { (void)ctx; return -1; }
int xmlNanoFTPCloseConnection(void* ctx) { (void)ctx; return -1; }
int xmlNanoFTPList(void* ctx, ftpListCallback callback, void* userData, const char* filename) {
    (void)ctx; (void)callback; (void)userData; (void)filename; return -1;
}
int xmlNanoFTPGetSocket(void* ctx, const char* filename) { (void)ctx; (void)filename; return -1; }
int xmlNanoFTPGet(void* ctx, ftpDataCallback callback, void* userData, const char* filename) {
    (void)ctx; (void)callback; (void)userData; (void)filename; return -1;
}
int xmlNanoFTPRead(void* ctx, void* dest, int len) { (void)ctx; (void)dest; (void)len; return -1; }
