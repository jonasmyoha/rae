/* TCP sockets for lib/net (docs/server-benchmarks-design.md §9 G1, F1).
 *
 * Rae sees value handles and status codes only: no errno, no sockaddr, no
 * fcntl. A handle is the socket's file descriptor (>= 0); every function that
 * can fail answers a status, and lib/net turns the codes into its NetStatus
 * enum. Every socket is NON-BLOCKING: an operation that would wait answers
 * NET_WOULD_BLOCK, and the caller waits on readiness (G2's poller, or
 * rae_ext_Net_waitReadable for a single socket).
 *
 * Options set here, so no server forgets them: SO_REUSEADDR on listeners (a
 * restarted server can rebind at once), TCP_NODELAY on connected sockets
 * (small responses go out without Nagle's delay), and no SIGPIPE on a write
 * to a closed peer (SO_NOSIGPIPE on macOS, MSG_NOSIGNAL on Linux) — that is a
 * status, not a dead process.
 *
 * macOS and Linux (F8: macOS first). Elsewhere every call answers
 * NET_UNSUPPORTED. */

#define NET_OK 0
#define NET_WOULD_BLOCK -1
#define NET_END_OF_STREAM -2
#define NET_ADDRESS_IN_USE -3
#define NET_CONNECTION_REFUSED -4
#define NET_CONNECTION_RESET -5
#define NET_INVALID_ADDRESS -6
#define NET_TOO_MANY_OPEN -7
#define NET_UNSUPPORTED -8
#define NET_FAILED -9

#if (defined(__APPLE__) || defined(__linux__)) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static int64_t rae_net_status_from_errno(int code) {
  switch (code) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EINPROGRESS:
      return NET_WOULD_BLOCK;
    case EADDRINUSE: return NET_ADDRESS_IN_USE;
    case ECONNREFUSED: return NET_CONNECTION_REFUSED;
    case ECONNRESET:
    case EPIPE:
      return NET_CONNECTION_RESET;
    case EADDRNOTAVAIL:
    case EAFNOSUPPORT:
      return NET_INVALID_ADDRESS;
    case EMFILE:
    case ENFILE:
      return NET_TOO_MANY_OPEN;
    default: return NET_FAILED;
  }
}

static int rae_net_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* A connected socket's options: no Nagle delay, no SIGPIPE, close-on-exec. */
static void rae_net_prepare_connected(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

/* host + port -> address list; host "" means every local address. */
static int64_t rae_net_resolve(rae_String host, int64_t port, int passive, struct addrinfo** out) {
  if (port < 0 || port > 65535) return NET_INVALID_ADDRESS;
  char service[16];
  snprintf(service, sizeof service, "%lld", (long long)port);
  char name[256];
  const char* node = NULL;
  if (host.data && host.len > 0) {
    if (host.len >= (int64_t)sizeof name) return NET_INVALID_ADDRESS;
    memcpy(name, host.data, (size_t)host.len);
    name[host.len] = '\0';
    node = name;
  }
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  if (getaddrinfo(node, service, &hints, out) != 0) return NET_INVALID_ADDRESS;
  return NET_OK;
}

/* Listen on host:port (port 0 picks a free port; socketLocalPort tells which).
 * Answers the handle (>= 0) or a status (< 0). */
int64_t rae_ext_Net_listen(rae_String host, int64_t port, int64_t backlog) {
  struct addrinfo* addresses = NULL;
  int64_t status = rae_net_resolve(host, port, 1, &addresses);
  if (status != NET_OK) return status;
  int64_t result = NET_INVALID_ADDRESS;
  for (struct addrinfo* a = addresses; a; a = a->ai_next) {
    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) { result = rae_net_status_from_errno(errno); continue; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (bind(fd, a->ai_addr, a->ai_addrlen) != 0
        || listen(fd, backlog > 0 ? (int)backlog : SOMAXCONN) != 0
        || rae_net_set_nonblocking(fd) != 0) {
      result = rae_net_status_from_errno(errno);
      close(fd);
      continue;
    }
    result = fd;
    break;
  }
  freeaddrinfo(addresses);
  return result;
}

/* Accept one pending connection: its handle, or NET_WOULD_BLOCK when none. */
int64_t rae_ext_Net_accept(int64_t listener) {
  if (listener < 0) return NET_FAILED;
  for (;;) {
    int fd = accept((int)listener, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR) continue;
      /* A peer that gave up before we accepted it is not the listener's
       * failure: report "nothing to accept" and let the caller go on. */
      if (errno == ECONNABORTED) return NET_WOULD_BLOCK;
      return rae_net_status_from_errno(errno);
    }
    if (rae_net_set_nonblocking(fd) != 0) {
      close(fd);
      return NET_FAILED;
    }
    rae_net_prepare_connected(fd);
    return fd;
  }
}

/* Connect to host:port. Waits up to timeoutMs for the connection, then
 * answers a non-blocking handle (>= 0) or a status. */
int64_t rae_ext_Net_connect(rae_String host, int64_t port, int64_t timeoutMs) {
  struct addrinfo* addresses = NULL;
  int64_t status = rae_net_resolve(host, port, 0, &addresses);
  if (status != NET_OK) return status;
  int64_t result = NET_CONNECTION_REFUSED;
  for (struct addrinfo* a = addresses; a; a = a->ai_next) {
    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) { result = rae_net_status_from_errno(errno); continue; }
    if (rae_net_set_nonblocking(fd) != 0) { close(fd); result = NET_FAILED; continue; }
    int code = 0;
    if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
      if (errno != EINPROGRESS) {
        result = rae_net_status_from_errno(errno);
        close(fd);
        continue;
      }
      struct pollfd waitFor = { .fd = fd, .events = POLLOUT, .revents = 0 };
      int ready = poll(&waitFor, 1, timeoutMs < 0 ? -1 : (int)timeoutMs);
      if (ready <= 0) { result = NET_WOULD_BLOCK; close(fd); continue; }
      socklen_t length = sizeof code;
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &length);
      if (code != 0) { result = rae_net_status_from_errno(code); close(fd); continue; }
    }
    rae_net_prepare_connected(fd);
    result = fd;
    break;
  }
  freeaddrinfo(addresses);
  return result;
}

/* Read up to maxBytes into buffer[offset...]: the byte count (> 0),
 * NET_END_OF_STREAM when the peer closed, or a status. */
int64_t rae_ext_Net_read(int64_t handle, uint8_t* buffer, int64_t offset, int64_t maxBytes) {
  if (handle < 0 || !buffer || maxBytes <= 0) return NET_FAILED;
  for (;;) {
    ssize_t got = recv((int)handle, buffer + offset, (size_t)maxBytes, 0);
    if (got > 0) return (int64_t)got;
    if (got == 0) return NET_END_OF_STREAM;
    if (errno == EINTR) continue;
    return rae_net_status_from_errno(errno);
  }
}

/* Write count bytes from buffer[offset...]: the bytes written (possibly fewer
 * than asked when the socket's send buffer is full) or a status. */
int64_t rae_ext_Net_write(int64_t handle, uint8_t* buffer, int64_t offset, int64_t count) {
  if (handle < 0 || (!buffer && count > 0)) return NET_FAILED;
  if (count <= 0) return 0;
#ifdef MSG_NOSIGNAL
  int flags = MSG_NOSIGNAL;
#else
  int flags = 0;
#endif
  for (;;) {
    ssize_t sent = send((int)handle, buffer + offset, (size_t)count, flags);
    if (sent >= 0) return (int64_t)sent;
    if (errno == EINTR) continue;
    return rae_net_status_from_errno(errno);
  }
}

/* Write a String's bytes (the text convenience for small messages). */
int64_t rae_ext_Net_writeText(int64_t handle, rae_String text) {
  return rae_ext_Net_write(handle, text.data, 0, text.len);
}

/* Wait until the socket has data (or the peer closed): true, or false on
 * timeout. For one socket; many sockets use the poller. */
rae_Bool rae_ext_Net_waitReadable(int64_t handle, int64_t timeoutMs) {
  if (handle < 0) return 0;
  struct pollfd waitFor = { .fd = (int)handle, .events = POLLIN, .revents = 0 };
  for (;;) {
    int ready = poll(&waitFor, 1, timeoutMs < 0 ? -1 : (int)timeoutMs);
    if (ready < 0 && errno == EINTR) continue;
    return ready > 0;
  }
}

/* The local port a socket is bound to (a listener on port 0 asks this). */
int64_t rae_ext_Net_localPort(int64_t handle) {
  if (handle < 0) return -1;
  struct sockaddr_storage address;
  socklen_t length = sizeof address;
  if (getsockname((int)handle, (struct sockaddr*)&address, &length) != 0) return -1;
  if (address.ss_family == AF_INET) return ntohs(((struct sockaddr_in*)&address)->sin_port);
  if (address.ss_family == AF_INET6) return ntohs(((struct sockaddr_in6*)&address)->sin6_port);
  return -1;
}

void rae_ext_Net_close(int64_t handle) {
  if (handle >= 0) close((int)handle);
}

/* Bytes buffer[offset .. offset+count) as an owned String. */
rae_String rae_ext_Net_bytesToText(uint8_t* buffer, int64_t offset, int64_t count) {
  if (!buffer || count <= 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* data = malloc((size_t)count + 1);
  if (!data) return (rae_String){NULL, 0, 0, 0};
  memcpy(data, buffer + offset, (size_t)count);
  data[count] = '\0';
  rae_mem_str_tag(data, count + 1, RAE_SITE_CONCAT);
  rae_string_pool_register(data);
  return (rae_String){data, count, count + 1, 1};
}

#else

int64_t rae_ext_Net_listen(rae_String host, int64_t port, int64_t backlog) {
  (void)host; (void)port; (void)backlog;
  return NET_UNSUPPORTED;
}
int64_t rae_ext_Net_accept(int64_t listener) { (void)listener; return NET_UNSUPPORTED; }
int64_t rae_ext_Net_connect(rae_String host, int64_t port, int64_t timeoutMs) {
  (void)host; (void)port; (void)timeoutMs;
  return NET_UNSUPPORTED;
}
int64_t rae_ext_Net_read(int64_t handle, uint8_t* buffer, int64_t offset, int64_t maxBytes) {
  (void)handle; (void)buffer; (void)offset; (void)maxBytes;
  return NET_UNSUPPORTED;
}
int64_t rae_ext_Net_write(int64_t handle, uint8_t* buffer, int64_t offset, int64_t count) {
  (void)handle; (void)buffer; (void)offset; (void)count;
  return NET_UNSUPPORTED;
}
int64_t rae_ext_Net_writeText(int64_t handle, rae_String text) {
  (void)handle; (void)text;
  return NET_UNSUPPORTED;
}
rae_Bool rae_ext_Net_waitReadable(int64_t handle, int64_t timeoutMs) {
  (void)handle; (void)timeoutMs;
  return 0;
}
int64_t rae_ext_Net_localPort(int64_t handle) { (void)handle; return -1; }
void rae_ext_Net_close(int64_t handle) { (void)handle; }
rae_String rae_ext_Net_bytesToText(uint8_t* buffer, int64_t offset, int64_t count) {
  (void)buffer; (void)offset; (void)count;
  return (rae_String){NULL, 0, 0, 0};
}

#endif
