/* Browser WebSockets for lib/webSocket/WebSocketBrowser.rae (the WASM build;
 * docs/relay.md, "WebSocket"). A browser has no TCP socket: the page's own
 * WebSocket object does the connect, the handshake and the framing, through
 * Emscripten's emscripten/websocket.h (linked with -lwebsocket.js when the
 * program uses these; compiler/src/main.c).
 *
 * Platform reason (AGENTS.md, runtime C rule): the browser API itself. The
 * browser hands messages to a callback between frames, so the one piece of
 * glue that is not a single call is a byte queue per socket that the
 * onmessage callback fills ([4-byte length][bytes] per binary message) and
 * Rae drains each frame. Text messages are dropped (relay packets are
 * binary). The calls hold no policy: no retries, no reconnects.
 *
 * Single-threaded: a page's WebSocket callbacks run on the main thread, the
 * thread that calls these. Native builds compile none of it. */
#ifdef __EMSCRIPTEN__
#include <emscripten/websocket.h>

#define RAE_WEB_SOCKET_MAX 16

typedef struct {
  EMSCRIPTEN_WEBSOCKET_T socket;
  int used;
  /* 0 connecting, 1 open, 2 closed (or failed) */
  int state;
  uint8_t* queue;
  size_t queueStart;
  size_t queueLength;
  size_t queueCapacity;
} RaeWebSocket;

static RaeWebSocket g_rae_web_sockets[RAE_WEB_SOCKET_MAX];

static RaeWebSocket* rae_web_socket_at(int64_t handle) {
  if (handle < 1 || handle > RAE_WEB_SOCKET_MAX) return NULL;
  RaeWebSocket* slot = &g_rae_web_sockets[handle - 1];
  return slot->used ? slot : NULL;
}

static EM_BOOL rae_web_socket_on_open(int type, const EmscriptenWebSocketOpenEvent* event, void* user) {
  RaeWebSocket* slot = (RaeWebSocket*)user;
  if (slot->state == 0) slot->state = 1;
  return EM_TRUE;
}

static EM_BOOL rae_web_socket_on_close(int type, const EmscriptenWebSocketCloseEvent* event, void* user) {
  ((RaeWebSocket*)user)->state = 2;
  return EM_TRUE;
}

static EM_BOOL rae_web_socket_on_error(int type, const EmscriptenWebSocketErrorEvent* event, void* user) {
  ((RaeWebSocket*)user)->state = 2;
  return EM_TRUE;
}

static EM_BOOL rae_web_socket_on_message(int type, const EmscriptenWebSocketMessageEvent* event, void* user) {
  RaeWebSocket* slot = (RaeWebSocket*)user;
  if (event->isText) return EM_TRUE;
  size_t need = slot->queueLength + 4 + event->numBytes;
  if (need > slot->queueCapacity) {
    /* Room by moving the unread part to the front first, then by growing */
    if (slot->queueStart > 0) {
      memmove(slot->queue, slot->queue + slot->queueStart, slot->queueLength - slot->queueStart);
      slot->queueLength -= slot->queueStart;
      slot->queueStart = 0;
      need = slot->queueLength + 4 + event->numBytes;
    }
    if (need > slot->queueCapacity) {
      size_t capacity = slot->queueCapacity ? slot->queueCapacity * 2 : 65536;
      while (capacity < need) capacity *= 2;
      uint8_t* grown = (uint8_t*)realloc(slot->queue, capacity);
      if (!grown) return EM_TRUE;
      slot->queue = grown;
      slot->queueCapacity = capacity;
    }
  }
  uint32_t length = event->numBytes;
  uint8_t* at = slot->queue + slot->queueLength;
  at[0] = (uint8_t)(length >> 24);
  at[1] = (uint8_t)(length >> 16);
  at[2] = (uint8_t)(length >> 8);
  at[3] = (uint8_t)length;
  memcpy(at + 4, event->data, length);
  slot->queueLength += 4 + length;
  return EM_TRUE;
}

/* Open a WebSocket to `url` (ws://host:port/path): a handle >= 1, or -1 */
int64_t rae_ext_WebSocketWeb_open(rae_String url) {
  if (!url.data || !emscripten_websocket_is_supported()) return -1;
  int index = -1;
  for (int i = 0; i < RAE_WEB_SOCKET_MAX; i++) {
    if (!g_rae_web_sockets[i].used) { index = i; break; }
  }
  if (index < 0) return -1;
  char text[1024];
  size_t count = url.len < sizeof(text) - 1 ? (size_t)url.len : sizeof(text) - 1;
  memcpy(text, url.data, count);
  text[count] = 0;
  EmscriptenWebSocketCreateAttributes attributes;
  emscripten_websocket_init_create_attributes(&attributes);
  attributes.url = text;
  attributes.protocols = NULL;
  attributes.createOnMainThread = EM_TRUE;
  EMSCRIPTEN_WEBSOCKET_T socket = emscripten_websocket_new(&attributes);
  if (socket <= 0) return -1;
  RaeWebSocket* slot = &g_rae_web_sockets[index];
  memset(slot, 0, sizeof(*slot));
  slot->socket = socket;
  slot->used = 1;
  emscripten_websocket_set_onopen_callback(socket, slot, rae_web_socket_on_open);
  emscripten_websocket_set_onclose_callback(socket, slot, rae_web_socket_on_close);
  emscripten_websocket_set_onerror_callback(socket, slot, rae_web_socket_on_error);
  emscripten_websocket_set_onmessage_callback(socket, slot, rae_web_socket_on_message);
  return index + 1;
}

/* 0 connecting, 1 open, 2 closed; -1 for no such socket */
int64_t rae_ext_WebSocketWeb_state(int64_t handle) {
  RaeWebSocket* slot = rae_web_socket_at(handle);
  return slot ? slot->state : -1;
}

/* Send `count` bytes from `offset` as one binary message: 0, or -1 */
int64_t rae_ext_WebSocketWeb_send(int64_t handle, uint8_t* buffer, int64_t offset, int64_t count) {
  RaeWebSocket* slot = rae_web_socket_at(handle);
  if (!slot || slot->state != 1) return -1;
  EMSCRIPTEN_RESULT result = emscripten_websocket_send_binary(slot->socket, buffer + offset, (uint32_t)count);
  return result == EMSCRIPTEN_RESULT_SUCCESS ? 0 : -1;
}

/* The length of the next queued message, or -1 when none is waiting */
int64_t rae_ext_WebSocketWeb_nextLength(int64_t handle) {
  RaeWebSocket* slot = rae_web_socket_at(handle);
  if (!slot || slot->queueLength - slot->queueStart < 4) return -1;
  uint8_t* at = slot->queue + slot->queueStart;
  return ((int64_t)at[0] << 24) | ((int64_t)at[1] << 16) | ((int64_t)at[2] << 8) | (int64_t)at[3];
}

/* Copy the next queued message into `out` (room for nextLength bytes) and
 * drop it from the queue: its length, or -1 */
int64_t rae_ext_WebSocketWeb_take(int64_t handle, uint8_t* out) {
  int64_t length = rae_ext_WebSocketWeb_nextLength(handle);
  if (length < 0) return -1;
  RaeWebSocket* slot = rae_web_socket_at(handle);
  memcpy(out, slot->queue + slot->queueStart + 4, (size_t)length);
  slot->queueStart += 4 + (size_t)length;
  if (slot->queueStart == slot->queueLength) {
    slot->queueStart = 0;
    slot->queueLength = 0;
  }
  return length;
}

/* Close (1000) and free the socket and its queue */
void rae_ext_WebSocketWeb_close(int64_t handle) {
  RaeWebSocket* slot = rae_web_socket_at(handle);
  if (!slot) return;
  emscripten_websocket_close(slot->socket, 1000, "");
  emscripten_websocket_delete(slot->socket);
  free(slot->queue);
  memset(slot, 0, sizeof(*slot));
}
#endif
