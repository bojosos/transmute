/* Project-authored fixture: drives the guest libcurl runtime (/usr/lib/libcurl.4.dylib built
 * by scripts/build_alchemy_libcurl.ps1). A guest thread serves one HTTP request on a loopback
 * socket; the main thread fetches it with the easy interface (resolving "localhost" through
 * the threaded resolver), posts a body through the multi interface, reads a file:// URL, and
 * checks that https:// fails as unsupported (this build has no TLS backend).
 * Compiled against curl's public headers (third_party/curl) and runtime/include. */
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct Buffer { char data[512]; size_t length; };

static size_t collect(char* bytes, size_t size, size_t count, void* user) {
    struct Buffer* buffer = user;
    size_t length = size * count;
    for (size_t index = 0; index < length && buffer->length + 1 < sizeof buffer->data; ++index)
        buffer->data[buffer->length++] = bytes[index];
    buffer->data[buffer->length] = 0;
    return length;
}

static int listener;

/* Answers two requests: echoes the method, path and body length of each. */
static void* serve(void* unused) {
    (void)unused;
    for (int request = 0; request < 2; ++request) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) return NULL;
        char text[2048];
        size_t used = 0;
        long body = -1, content_length = 0;
        while (used + 1 < sizeof text) {
            ssize_t got = read(client, text + used, sizeof text - 1 - used);
            if (got <= 0) break;
            used += (size_t)got;
            text[used] = 0;
            char* end = strstr(text, "\r\n\r\n");
            if (end) {
                char* header = strstr(text, "Content-Length: ");
                if (header) sscanf(header + 16, "%ld", &content_length);
                body = (long)(used - (size_t)(end + 4 - text));
                if (body >= content_length) break;
            }
        }
        char method[8] = {0}, path[64] = {0};
        sscanf(text, "%7s %63s", method, path);
        char reply[256];
        char content[128];
        int content_size = snprintf(content, sizeof content, "%s %s body=%ld", method, path, body < 0 ? 0 : body);
        int reply_size = snprintf(reply, sizeof reply,
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
            content_size, content);
        write(client, reply, (size_t)reply_size);
        close(client);
    }
    return NULL;
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    printf("curl %s ssl %s ipv6 %s threaded %s\n", info->version, info->ssl_version ? info->ssl_version : "none",
           (info->features & CURL_VERSION_IPV6) ? "yes" : "no", (info->features & CURL_VERSION_ASYNCHDNS) ? "yes" : "no");

    listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, (struct sockaddr*)&address, sizeof address);
    listen(listener, 4);
    socklen_t length = sizeof address;
    getsockname(listener, (struct sockaddr*)&address, &length);
    int port = ntohs(address.sin_port);
    pthread_t server;
    pthread_create(&server, NULL, serve, NULL);

    char url[96];
    snprintf(url, sizeof url, "http://localhost:%d/hello?x=1", port);
    struct Buffer got = {{0}, 0};
    CURL* easy = curl_easy_init();
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &got);
    CURLcode code = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    printf("easy get: %s status %ld body \"%s\"\n", curl_easy_strerror(code), status, got.data);
    curl_easy_cleanup(easy);

    /* A POST through the multi interface, as cargo drives its transfers. */
    struct Buffer posted = {{0}, 0};
    snprintf(url, sizeof url, "http://127.0.0.1:%d/submit", port);
    CURL* transfer = curl_easy_init();
    curl_easy_setopt(transfer, CURLOPT_URL, url);
    curl_easy_setopt(transfer, CURLOPT_POSTFIELDS, "name=alchemy&tool=curl");
    curl_easy_setopt(transfer, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(transfer, CURLOPT_WRITEDATA, &posted);
    struct curl_slist* headers = curl_slist_append(NULL, "X-Fixture: yes");
    curl_easy_setopt(transfer, CURLOPT_HTTPHEADER, headers);
    CURLM* multi = curl_multi_init();
    curl_multi_add_handle(multi, transfer);
    int running = 1, rounds = 0;
    while (running && rounds++ < 10000) {
        curl_multi_perform(multi, &running);
        if (running) curl_multi_wait(multi, NULL, 0, 100, NULL);
    }
    int queued = 0;
    CURLMsg* message = curl_multi_info_read(multi, &queued);
    printf("multi post: %s body \"%s\"\n",
           message && message->msg == CURLMSG_DONE ? curl_easy_strerror(message->data.result) : "not done", posted.data);
    curl_multi_remove_handle(multi, transfer);
    curl_easy_cleanup(transfer);
    curl_slist_free_all(headers);
    curl_multi_cleanup(multi);
    pthread_join(server, NULL);
    close(listener);

    /* file:// reads through the FILE protocol. */
    FILE* file = fopen("/tmp/alchemy-curl-fixture.txt", "w");
    fputs("from a file\n", file);
    fclose(file);
    struct Buffer local = {{0}, 0};
    CURL* reader = curl_easy_init();
    curl_easy_setopt(reader, CURLOPT_URL, "file:///tmp/alchemy-curl-fixture.txt");
    curl_easy_setopt(reader, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(reader, CURLOPT_WRITEDATA, &local);
    code = curl_easy_perform(reader);
    printf("file: %s body \"%.11s\"\n", curl_easy_strerror(code), local.data);
    curl_easy_cleanup(reader);
    unlink("/tmp/alchemy-curl-fixture.txt");

    /* No TLS backend: https fails explicitly. */
    CURL* secure = curl_easy_init();
    curl_easy_setopt(secure, CURLOPT_URL, "https://127.0.0.1:1/");
    code = curl_easy_perform(secure);
    printf("https: %s\n", curl_easy_strerror(code));
    curl_easy_cleanup(secure);
    curl_global_cleanup();
    return 0;
}
