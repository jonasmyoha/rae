/* The socket system-call shim for lib/net (AGENTS.md "Runtime C rule";
 * docs/server-benchmarks-design.md §1.2).
 *
 * Each function makes ONE call and holds no policy: it returns the call's
 * result, or the negated errno. The listen sequence, retrying an interrupted
 * call, connect-with-timeout, which options a socket gets and what an error
 * means are all Rae (lib/net/NetSystem.rae binds these; lib/net/Tcp.rae is
 * the policy). C is needed here only for what the platform forces: errno,
 * the variadic fcntl, per-OS address structs (kept opaque in fixed-size
 * records) and per-OS constants (rae_ext_NetSys_constant).
 *
 * rae_ext_NetSys_bytesToText is not a system call: it allocates a rae_String,
 * which is the compiler's runtime ABI (Rae has no byte-level string builder
 * yet; docs/runtime-c-audit.md).
 *
 * macOS, Linux and the BSDs (the BSDs are not built or tested here yet).
 * Elsewhere every socket call answers RAE_NET_UNSUPPORTED. The readiness
 * poller (kqueue) exists only under RAE_HAS_KQUEUE, the C twin of
 * NetSystem.rae's `hasKqueue`: the Rae side declares it only there, so no
 * stub is needed elsewhere. */

/* Answers that are not a negated errno (errno values are small positives) */
#define RAE_NET_UNSUPPORTED -1000000
#define RAE_NET_RESOLVE_FAILED -1000001

/* One resolved address: family, socket type, protocol, address length, then
 * the address itself (opaque to Rae; its layout differs per OS). */
#define RAE_NET_RECORD_HEADER 16
#define RAE_NET_RECORD_BYTES (RAE_NET_RECORD_HEADER + 128)
/* The most events one pollerWait call reports (its kevent array lives on the
 * stack: per-OS struct layout, no allocation per wait) */
#define RAE_NET_MAX_WAIT_EVENTS 256

#if (defined(__APPLE__) || defined(__linux__) || defined(RAE_HAS_KQUEUE)) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef RAE_HAS_KQUEUE
#include <sys/event.h>
#endif

/* The constants lib/net needs, by the index NetSystem.rae names them with */
int64_t rae_ext_NetSys_constant(int64_t which) {
  switch (which) {
    case 0: return EAGAIN;
    case 1: return EWOULDBLOCK;
    case 2: return EINTR;
    case 3: return EINPROGRESS;
    case 4: return EADDRINUSE;
    case 5: return ECONNREFUSED;
    case 6: return ECONNRESET;
    case 7: return EPIPE;
    case 8: return EADDRNOTAVAIL;
    case 9: return EAFNOSUPPORT;
    case 10: return EMFILE;
    case 11: return ENFILE;
    case 12: return ECONNABORTED;
    case 13: return SOL_SOCKET;
    case 14: return SO_REUSEADDR;
#ifdef SO_NOSIGPIPE
    case 15: return SO_NOSIGPIPE;
#else
    case 15: return -1;
#endif
    case 16: return IPPROTO_TCP;
    case 17: return TCP_NODELAY;
    case 18: return O_NONBLOCK;
#ifdef MSG_NOSIGNAL
    case 19: return MSG_NOSIGNAL;
#else
    case 19: return 0;
#endif
    case 20: return POLLIN;
    case 21: return POLLOUT;
    case 22: return RAE_NET_RECORD_BYTES;
    case 23: return RAE_NET_RESOLVE_FAILED;
    case 24: return RAE_NET_UNSUPPORTED;
    case 25: return SOMAXCONN;
#ifdef RAE_HAS_KQUEUE
    case 26: return EVFILT_READ;
    case 27: return EVFILT_WRITE;
    case 28: return EV_ADD | EV_ENABLE;
    case 29: return EV_DELETE;
    case 30: return EV_EOF;
    case 31: return EV_ERROR;
#endif
    case 32: return RAE_NET_MAX_WAIT_EVENTS;
    default: return -1;
  }
}

/* getaddrinfo for TCP: writes up to maxRecords records into `records` and
 * answers how many, or RAE_NET_RESOLVE_FAILED. host "" = any local address. */
int64_t rae_ext_NetSys_resolve(rae_String host, int64_t port, rae_Bool passive, uint8_t* records, int64_t maxRecords) {
  char service[16];
  snprintf(service, sizeof service, "%lld", (long long)port);
  char name[256];
  const char* node = NULL;
  if (host.data && host.len > 0) {
    if (host.len >= (int64_t)sizeof name) return RAE_NET_RESOLVE_FAILED;
    memcpy(name, host.data, (size_t)host.len);
    name[host.len] = '\0';
    node = name;
  }
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  struct addrinfo* addresses = NULL;
  if (getaddrinfo(node, service, &hints, &addresses) != 0) return RAE_NET_RESOLVE_FAILED;
  int64_t count = 0;
  for (struct addrinfo* a = addresses; a && count < maxRecords; a = a->ai_next) {
    if (a->ai_addrlen > RAE_NET_RECORD_BYTES - RAE_NET_RECORD_HEADER) continue;
    uint8_t* record = records + count * RAE_NET_RECORD_BYTES;
    int32_t header[4] = { a->ai_family, a->ai_socktype, a->ai_protocol, (int32_t)a->ai_addrlen };
    memcpy(record, header, sizeof header);
    memcpy(record + RAE_NET_RECORD_HEADER, a->ai_addr, a->ai_addrlen);
    count++;
  }
  freeaddrinfo(addresses);
  return count;
}

static const int32_t* rae_net_record_header(const uint8_t* records, int64_t index) {
  return (const int32_t*)(const void*)(records + index * RAE_NET_RECORD_BYTES);
}

int64_t rae_ext_NetSys_socketFor(uint8_t* records, int64_t index) {
  const int32_t* header = rae_net_record_header(records, index);
  int fd = socket(header[0], header[1], header[2]);
  return fd >= 0 ? fd : -errno;
}

int64_t rae_ext_NetSys_bindTo(int64_t fd, uint8_t* records, int64_t index) {
  const int32_t* header = rae_net_record_header(records, index);
  const uint8_t* address = records + index * RAE_NET_RECORD_BYTES + RAE_NET_RECORD_HEADER;
  return bind((int)fd, (const struct sockaddr*)(const void*)address, (socklen_t)header[3]) == 0 ? 0 : -errno;
}

int64_t rae_ext_NetSys_connectTo(int64_t fd, uint8_t* records, int64_t index) {
  const int32_t* header = rae_net_record_header(records, index);
  const uint8_t* address = records + index * RAE_NET_RECORD_BYTES + RAE_NET_RECORD_HEADER;
  return connect((int)fd, (const struct sockaddr*)(const void*)address, (socklen_t)header[3]) == 0 ? 0 : -errno;
}

int64_t rae_ext_NetSys_listen(int64_t fd, int64_t backlog) {
  return listen((int)fd, (int)backlog) == 0 ? 0 : -errno;
}

int64_t rae_ext_NetSys_accept(int64_t fd) {
  int accepted = accept((int)fd, NULL, NULL);
  return accepted >= 0 ? accepted : -errno;
}

/* fcntl is variadic: these two are why a plain extern cannot do this */
int64_t rae_ext_NetSys_getFileFlags(int64_t fd) {
  int flags = fcntl((int)fd, F_GETFL, 0);
  return flags >= 0 ? flags : -errno;
}

int64_t rae_ext_NetSys_setFileFlags(int64_t fd, int64_t flags) {
  return fcntl((int)fd, F_SETFL, (int)flags) == 0 ? 0 : -errno;
}

int64_t rae_ext_NetSys_setCloseOnExec(int64_t fd) {
  return fcntl((int)fd, F_SETFD, FD_CLOEXEC) == 0 ? 0 : -errno;
}

int64_t rae_ext_NetSys_setIntOption(int64_t fd, int64_t level, int64_t option, int64_t value) {
  int optionValue = (int)value;
  return setsockopt((int)fd, (int)level, (int)option, &optionValue, sizeof optionValue) == 0 ? 0 : -errno;
}

/* The socket's pending error (SO_ERROR): 0, the error number, or -errno */
int64_t rae_ext_NetSys_socketError(int64_t fd) {
  int code = 0;
  socklen_t length = sizeof code;
  return getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &code, &length) == 0 ? code : -errno;
}

/* bytes read (0 = the peer closed) or -errno */
int64_t rae_ext_NetSys_receive(int64_t fd, uint8_t* buffer, int64_t offset, int64_t maxBytes) {
  ssize_t got = recv((int)fd, buffer + offset, (size_t)maxBytes, 0);
  return got >= 0 ? (int64_t)got : -errno;
}

int64_t rae_ext_NetSys_send(int64_t fd, uint8_t* buffer, int64_t offset, int64_t count, int64_t flags) {
  ssize_t sent = send((int)fd, buffer + offset, (size_t)count, (int)flags);
  return sent >= 0 ? (int64_t)sent : -errno;
}

int64_t rae_ext_NetSys_sendText(int64_t fd, rae_String text, int64_t flags) {
  ssize_t sent = send((int)fd, text.data, (size_t)text.len, (int)flags);
  return sent >= 0 ? (int64_t)sent : -errno;
}

/* poll() on one socket: > 0 ready, 0 timed out, -errno */
int64_t rae_ext_NetSys_pollOne(int64_t fd, int64_t events, int64_t timeoutMs) {
  struct pollfd waitFor = { .fd = (int)fd, .events = (short)events, .revents = 0 };
  int ready = poll(&waitFor, 1, (int)timeoutMs);
  return ready >= 0 ? ready : -errno;
}

int64_t rae_ext_NetSys_close(int64_t fd) {
  return close((int)fd) == 0 ? 0 : -errno;
}

/* The readiness poller (lib/net/Poller.rae): kqueue, under `hasKqueue`. A
 * Linux kernel gets epoll behind the same Rae API (F8, `hasEpoll`). */
#ifdef RAE_HAS_KQUEUE
int64_t rae_ext_NetSys_pollerCreate(void) {
  int queue = kqueue();
  return queue >= 0 ? queue : -errno;
}

/* One interest change: `flags` (add / delete, from the constant table) for
 * `filter` (read / write) on `fd` */
int64_t rae_ext_NetSys_pollerChange(int64_t queue, int64_t fd, int64_t filter, int64_t flags) {
  struct kevent change;
  EV_SET(&change, (uintptr_t)fd, (int16_t)filter, (uint16_t)flags, 0, 0, NULL);
  return kevent((int)queue, &change, 1, NULL, 0, NULL) == 0 ? 0 : -errno;
}

/* Wait up to `timeoutMs` (negative: forever) for up to `maxEvents` events and
 * write each as (handle, filter, flags) into `events`: their count, or -errno */
int64_t rae_ext_NetSys_pollerWait(int64_t queue, int64_t* events, int64_t maxEvents, int64_t timeoutMs) {
  struct kevent ready[RAE_NET_MAX_WAIT_EVENTS];
  int capacity = maxEvents < RAE_NET_MAX_WAIT_EVENTS ? (int)maxEvents : RAE_NET_MAX_WAIT_EVENTS;
  struct timespec timeout = { (time_t)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000000) };
  int count = kevent((int)queue, NULL, 0, ready, capacity, timeoutMs < 0 ? NULL : &timeout);
  if (count < 0) return -errno;
  for (int i = 0; i < count; i++) {
    events[i * 3] = (int64_t)ready[i].ident;
    events[i * 3 + 1] = (int64_t)ready[i].filter;
    events[i * 3 + 2] = (int64_t)ready[i].flags;
  }
  return count;
}
#endif

/* getsockname, decoded per address family: the local port, or -errno */
int64_t rae_ext_NetSys_localPort(int64_t fd) {
  struct sockaddr_storage address;
  socklen_t length = sizeof address;
  if (getsockname((int)fd, (struct sockaddr*)&address, &length) != 0) return -errno;
  if (address.ss_family == AF_INET) return ntohs(((struct sockaddr_in*)&address)->sin_port);
  if (address.ss_family == AF_INET6) return ntohs(((struct sockaddr_in6*)&address)->sin6_port);
  return -EAFNOSUPPORT;
}

#else

int64_t rae_ext_NetSys_constant(int64_t which) {
  if (which == 22) return RAE_NET_RECORD_BYTES;
  if (which == 23) return RAE_NET_RESOLVE_FAILED;
  if (which == 24) return RAE_NET_UNSUPPORTED;
  if (which == 32) return RAE_NET_MAX_WAIT_EVENTS;
  return 0;
}
int64_t rae_ext_NetSys_resolve(rae_String host, int64_t port, rae_Bool passive, uint8_t* records, int64_t maxRecords) {
  (void)host; (void)port; (void)passive; (void)records; (void)maxRecords;
  return RAE_NET_UNSUPPORTED;
}
int64_t rae_ext_NetSys_socketFor(uint8_t* records, int64_t index) { (void)records; (void)index; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_bindTo(int64_t fd, uint8_t* records, int64_t index) { (void)fd; (void)records; (void)index; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_connectTo(int64_t fd, uint8_t* records, int64_t index) { (void)fd; (void)records; (void)index; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_listen(int64_t fd, int64_t backlog) { (void)fd; (void)backlog; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_accept(int64_t fd) { (void)fd; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_getFileFlags(int64_t fd) { (void)fd; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_setFileFlags(int64_t fd, int64_t flags) { (void)fd; (void)flags; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_setCloseOnExec(int64_t fd) { (void)fd; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_setIntOption(int64_t fd, int64_t level, int64_t option, int64_t value) {
  (void)fd; (void)level; (void)option; (void)value;
  return RAE_NET_UNSUPPORTED;
}
int64_t rae_ext_NetSys_socketError(int64_t fd) { (void)fd; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_receive(int64_t fd, uint8_t* buffer, int64_t offset, int64_t maxBytes) {
  (void)fd; (void)buffer; (void)offset; (void)maxBytes;
  return RAE_NET_UNSUPPORTED;
}
int64_t rae_ext_NetSys_send(int64_t fd, uint8_t* buffer, int64_t offset, int64_t count, int64_t flags) {
  (void)fd; (void)buffer; (void)offset; (void)count; (void)flags;
  return RAE_NET_UNSUPPORTED;
}
int64_t rae_ext_NetSys_sendText(int64_t fd, rae_String text, int64_t flags) { (void)fd; (void)text; (void)flags; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_pollOne(int64_t fd, int64_t events, int64_t timeoutMs) { (void)fd; (void)events; (void)timeoutMs; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_close(int64_t fd) { (void)fd; return RAE_NET_UNSUPPORTED; }
int64_t rae_ext_NetSys_localPort(int64_t fd) { (void)fd; return RAE_NET_UNSUPPORTED; }

#endif

/* Bytes buffer[offset .. offset+count) as an owned String (string ABI) */
rae_String rae_ext_NetSys_bytesToText(uint8_t* buffer, int64_t offset, int64_t count) {
  if (!buffer || count <= 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* data = malloc((size_t)count + 1);
  if (!data) return (rae_String){NULL, 0, 0, 0};
  memcpy(data, buffer + offset, (size_t)count);
  data[count] = '\0';
  rae_mem_str_tag(data, count + 1, RAE_SITE_CONCAT);
  rae_string_pool_register(data);
  return (rae_String){data, count, count + 1, 1};
}
