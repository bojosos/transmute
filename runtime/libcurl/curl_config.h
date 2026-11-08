/* Alchemy build configuration for the guest ARM64 libcurl runtime.
 *
 * Project-authored (MIT). Consumed by scripts/build_alchemy_libcurl.ps1 in place
 * of the configure/CMake-generated curl_config.h, for the Darwin LP64 profile
 * that runtime/include declares.
 *
 * Profile: HTTP (1.1, with proxies, cookies and the usual authentication
 * schemes) and FILE, IPv4 and IPv6, Unix domain sockets, and the threaded
 * resolver over libSystem's getaddrinfo. No TLS backend: curl removed Secure
 * Transport in 8.15 and Alchemy ships no other, so https:// and the other TLS
 * schemes fail with CURLE_UNSUPPORTED_PROTOCOL. No zlib, brotli, zstd, IDN,
 * HTTP/2, HTTP/3, SSH or LDAP; SMB needs a TLS library's crypto. The other
 * protocols are disabled.
 */
#ifndef ALCHEMY_CURL_CONFIG_H
#define ALCHEMY_CURL_CONFIG_H 1

#define CURL_OS "aarch64-apple-darwin"

/* Only the public API (CURL_EXTERN) is exported, as upstream's builds do; the
 * objects are compiled with -fvisibility=hidden. */
#define CURL_HIDDEN_SYMBOLS 1
#define CURL_EXTERN_SYMBOL __attribute__((__visibility__("default")))

/* Protocols. */
#define CURL_DISABLE_DICT 1
#define CURL_DISABLE_FTP 1
#define CURL_DISABLE_GOPHER 1
#define CURL_DISABLE_IMAP 1
#define CURL_DISABLE_IPFS 1
#define CURL_DISABLE_LDAP 1
#define CURL_DISABLE_LDAPS 1
#define CURL_DISABLE_MQTT 1
#define CURL_DISABLE_POP3 1
#define CURL_DISABLE_RTSP 1
#define CURL_DISABLE_SMTP 1
#define CURL_DISABLE_TELNET 1
#define CURL_DISABLE_TFTP 1
#define CURL_DISABLE_WEBSOCKETS 1

/* Network. */
#define USE_IPV6 1
#define USE_UNIX_SOCKETS 1
/* getaddrinfo on a resolver thread (pthreads). */
#define USE_RESOLV_THREADED 1
#define HAVE_THREADS_POSIX 1
#define HAVE_PTHREAD_H 1
#define HAVE_GETADDRINFO 1
#define HAVE_GETADDRINFO_THREADSAFE 1
#define HAVE_FREEADDRINFO 1
#define HAVE_SOCKET 1
#define HAVE_SOCKETPAIR 1
#define HAVE_PIPE 1
#define HAVE_POLL 1
#define HAVE_RECV 1
#define HAVE_SEND 1
#define HAVE_SENDMSG 1
#define HAVE_GETSOCKNAME 1
#define HAVE_GETPEERNAME 1
#define HAVE_IF_NAMETOINDEX 1
#define HAVE_GETHOSTNAME 1
#define HAVE_FCNTL 1
#define HAVE_FCNTL_O_NONBLOCK 1
#define HAVE_SA_FAMILY_T 1
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_SOCKADDR_IN6_SIN6_SCOPE_ID 1

/* Headers runtime/include provides. */
#define HAVE_ARPA_INET_H 1
#define HAVE_FCNTL_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_NET_IF_H 1
#define HAVE_POLL_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_UNISTD_H 1

/* C library. */
#define HAVE_ARC4RANDOM 1
#define HAVE_BOOL_T 1
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_STRUCT_TIMEVAL 1
#define HAVE_SIGNAL 1

/* Type sizes (LP64). */
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_CURL_OFF_T 8
#define SIZEOF_CURL_SOCKET_T 4
#define SIZEOF_SIZE_T 8
#define SIZEOF_TIME_T 8

#endif
