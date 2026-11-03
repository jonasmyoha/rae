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
 * macOS, Linux and the BSDs (the BSDs are not built or tested here yet):
 * RAE_HAS_POSIX_SOCKETS, the C twin of NetSystem.rae's `hasPosixSockets`.
 * Elsewhere the Rae side declares Rae functions of the same names that
 * answer the unsupported marker, so there are no C stubs. The readiness
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

#ifdef RAE_HAS_POSIX_SOCKETS

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#ifdef RAE_HAS_KQUEUE
#include <sys/event.h>
#endif
#ifdef RAE_HAS_EPOLL
#include <sys/epoll.h>
#include <sys/eventfd.h>
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
    case 41: return EVFILT_USER;
    case 42: return EV_ADD | EV_CLEAR;
#endif
    case 32: return RAE_NET_MAX_WAIT_EVENTS;
#ifdef RAE_HAS_EPOLL
    case 33: return EPOLLIN;
    case 34: return EPOLLOUT;
    case 35: return EPOLLRDHUP;
    case 36: return EPOLLHUP;
    case 37: return EPOLLERR;
    case 38: return EPOLL_CTL_ADD;
    case 39: return EPOLL_CTL_DEL;
#endif
    case 40: return EEXIST;
    case 43: return EMSGSIZE;
    default: return -1;
  }
}

/* getaddrinfo for TCP (or UDP when `datagram`): writes up to maxRecords
 * records into `records` and answers how many, or RAE_NET_RESOLVE_FAILED.
 * host "" = any local address. */
int64_t rae_ext_NetSys_resolve(rae_String host, int64_t port, rae_Bool passive, rae_Bool datagram, uint8_t* records, int64_t maxRecords) {
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
  hints.ai_socktype = datagram ? SOCK_DGRAM : SOCK_STREAM;
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

/* Trigger the user event `ident` a poller watches (EVFILT_USER, added with
 * EV_CLEAR so it resets once reported): what wakes a pollerWait from another
 * thread. kevent is thread-safe. */
int64_t rae_ext_NetSys_pollerTrigger(int64_t queue, int64_t ident) {
  struct kevent change;
  EV_SET(&change, (uintptr_t)ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
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

/* The readiness poller on a Linux kernel (lib/net/Poller.rae, `hasEpoll`):
 * the same Rae API over epoll. Rae owns the policy (a read queue and a write
 * queue per poller, the write queue nested in the read one, what an event
 * means); each function here is one epoll call. */
#ifdef RAE_HAS_EPOLL
int64_t rae_ext_NetSys_epollCreate(void) {
  int queue = epoll_create1(EPOLL_CLOEXEC);
  return queue >= 0 ? queue : -errno;
}

/* One epoll_ctl: `operation` (add / delete, from the constant table) of `fd`
 * with `events` */
int64_t rae_ext_NetSys_epollControl(int64_t queue, int64_t operation, int64_t fd, int64_t events) {
  struct epoll_event event;
  memset(&event, 0, sizeof event);
  event.events = (uint32_t)events;
  event.data.fd = (int)fd;
  return epoll_ctl((int)queue, (int)operation, (int)fd, &event) == 0 ? 0 : -errno;
}

/* A non-blocking eventfd: what wakes an epoll poller from another thread */
int64_t rae_ext_NetSys_eventFdCreate(void) {
  int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  return fd >= 0 ? fd : -errno;
}

/* Add 1 to the eventfd's counter, which makes it readable (thread-safe) */
int64_t rae_ext_NetSys_eventFdSignal(int64_t fd) {
  uint64_t one = 1;
  return write((int)fd, &one, sizeof one) == (ssize_t)sizeof one ? 0 : -errno;
}

/* Read the eventfd's counter back to 0: the count, or -errno (-EAGAIN: 0) */
int64_t rae_ext_NetSys_eventFdDrain(int64_t fd) {
  uint64_t count = 0;
  return read((int)fd, &count, sizeof count) == (ssize_t)sizeof count ? (int64_t)count : -errno;
}

/* Wait up to `timeoutMs` (negative: forever) for up to `maxEvents` events and
 * write each as (fd, events, 0) into `events`: their count, or -errno */
int64_t rae_ext_NetSys_epollWait(int64_t queue, int64_t* events, int64_t maxEvents, int64_t timeoutMs) {
  struct epoll_event ready[RAE_NET_MAX_WAIT_EVENTS];
  int capacity = maxEvents < RAE_NET_MAX_WAIT_EVENTS ? (int)maxEvents : RAE_NET_MAX_WAIT_EVENTS;
  int count = epoll_wait((int)queue, ready, capacity, timeoutMs < 0 ? -1 : (int)timeoutMs);
  if (count < 0) return -errno;
  for (int i = 0; i < count; i++) {
    events[i * 3] = (int64_t)ready[i].data.fd;
    events[i * 3 + 1] = (int64_t)ready[i].events;
    events[i * 3 + 2] = 0;
  }
  return count;
}
#endif

/* A UDP peer address crosses into Rae as five words (lib/net/Udp.rae's
 * NetAddress): family (4, 6, or 0 for none), the address as two big-endian
 * 64-bit halves (IPv4: all in `low`), the port and the IPv6 scope id. These
 * two helpers only convert between those words and the per-OS sockaddr
 * layout; they make no call. */
static void rae_net_words_from_address(const struct sockaddr_storage* address, int64_t* words) {
  memset(words, 0, 5 * sizeof(int64_t));
  if (address->ss_family == AF_INET) {
    const struct sockaddr_in* ipv4 = (const struct sockaddr_in*)(const void*)address;
    words[0] = 4;
    words[2] = (int64_t)ntohl(ipv4->sin_addr.s_addr);
    words[3] = ntohs(ipv4->sin_port);
  } else if (address->ss_family == AF_INET6) {
    const struct sockaddr_in6* ipv6 = (const struct sockaddr_in6*)(const void*)address;
    uint64_t high = 0, low = 0;
    for (int i = 0; i < 8; i++) high = (high << 8) | ipv6->sin6_addr.s6_addr[i];
    for (int i = 8; i < 16; i++) low = (low << 8) | ipv6->sin6_addr.s6_addr[i];
    words[0] = 6;
    words[1] = (int64_t)high;
    words[2] = (int64_t)low;
    words[3] = ntohs(ipv6->sin6_port);
    words[4] = (int64_t)ipv6->sin6_scope_id;
  }
}

static socklen_t rae_net_address_from_words(struct sockaddr_storage* address, int64_t family, int64_t high, int64_t low, int64_t port, int64_t scopeId) {
  memset(address, 0, sizeof *address);
  if (family == 4) {
    struct sockaddr_in* ipv4 = (struct sockaddr_in*)(void*)address;
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons((uint16_t)port);
    ipv4->sin_addr.s_addr = htonl((uint32_t)low);
    return sizeof *ipv4;
  }
  if (family == 6) {
    struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)(void*)address;
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons((uint16_t)port);
    ipv6->sin6_scope_id = (uint32_t)scopeId;
    for (int i = 0; i < 8; i++) ipv6->sin6_addr.s6_addr[i] = (uint8_t)((uint64_t)high >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) ipv6->sin6_addr.s6_addr[8 + i] = (uint8_t)((uint64_t)low >> (56 - 8 * i));
    return sizeof *ipv6;
  }
  return 0;
}

/* A resolved record's address as the five NetAddress words (no call) */
int64_t rae_ext_NetSys_recordAddress(uint8_t* records, int64_t index, int64_t* words) {
  const int32_t* header = rae_net_record_header(records, index);
  struct sockaddr_storage address;
  memset(&address, 0, sizeof address);
  size_t length = (size_t)header[3] < sizeof address ? (size_t)header[3] : sizeof address;
  memcpy(&address, records + index * RAE_NET_RECORD_BYTES + RAE_NET_RECORD_HEADER, length);
  rae_net_words_from_address(&address, words);
  return words[0] == 0 ? -EAFNOSUPPORT : 0;
}

/* sendto one datagram to the address given as words: bytes sent or -errno */
int64_t rae_ext_NetSys_sendTo(int64_t fd, uint8_t* buffer, int64_t offset, int64_t count, int64_t flags, int64_t family, int64_t high, int64_t low, int64_t port, int64_t scopeId) {
  struct sockaddr_storage address;
  socklen_t length = rae_net_address_from_words(&address, family, high, low, port, scopeId);
  if (length == 0) return -EAFNOSUPPORT;
  ssize_t sent = sendto((int)fd, buffer + offset, (size_t)count, (int)flags, (struct sockaddr*)&address, length);
  return sent >= 0 ? (int64_t)sent : -errno;
}

/* recvmsg one datagram into buffer[offset ..): the bytes kept, or -errno.
 * Writes the sender as words[0..4] and, in words[5], 1 when the datagram was
 * larger than maxBytes (the kernel cut it: MSG_TRUNC), else 0. */
int64_t rae_ext_NetSys_receiveFrom(int64_t fd, uint8_t* buffer, int64_t offset, int64_t maxBytes, int64_t* words) {
  struct sockaddr_storage address;
  memset(&address, 0, sizeof address);
  struct iovec part = { .iov_base = buffer + offset, .iov_len = (size_t)maxBytes };
  struct msghdr message;
  memset(&message, 0, sizeof message);
  message.msg_name = &address;
  message.msg_namelen = sizeof address;
  message.msg_iov = &part;
  message.msg_iovlen = 1;
  ssize_t got = recvmsg((int)fd, &message, 0);
  if (got < 0) return -errno;
  rae_net_words_from_address(&address, words);
  words[5] = (message.msg_flags & MSG_TRUNC) ? 1 : 0;
  return (int64_t)got;
}

/* getsockname, decoded per address family: the local port, or -errno */
int64_t rae_ext_NetSys_localPort(int64_t fd) {
  struct sockaddr_storage address;
  socklen_t length = sizeof address;
  if (getsockname((int)fd, (struct sockaddr*)&address, &length) != 0) return -errno;
  if (address.ss_family == AF_INET) return ntohs(((struct sockaddr_in*)&address)->sin_port);
  if (address.ss_family == AF_INET6) return ntohs(((struct sockaddr_in6*)&address)->sin6_port);
  return -EAFNOSUPPORT;
}

#endif  /* RAE_HAS_POSIX_SOCKETS */

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
