/* Apple's Network.framework for lib/net/NetworkConnection (docs/
 * network-connections.md): a client connection (TCP, TLS, or a WebSocket
 * client) that brings up the cellular radio and VPN on demand, follows the
 * network from Wi-Fi to cellular, checks TLS against the system trust store
 * and reports the path.
 *
 * Platform reason: Network.framework's C API (nw_*), which delivers
 * everything through blocks on a dispatch queue; Rae cannot pass a block.
 * So each connection has a small event queue here: the framework's handlers
 * (on the connection's own serial queue) append an event (ready, message,
 * closed, failed, waiting, path changed, send failed), signal a condition for
 * a waiting worker, and wake the UI loop (EventLoop.wake). Rae takes the
 * events one at a time (rae_ext_Nw_nextEvent, then the code, the length and
 * a copy of the bytes) and decides what they mean; it sends, and closes. No
 * other policy lives here.
 *
 * Header-only: the generated C of a program whose Rae imports the module
 * includes it (`cheader`), and the compiler then links -framework Network,
 * so other programs neither compile nor link it. Handles are integers (a
 * pointer as int64_t, 0 = none), as in the sql package. */
#ifndef RAE_RUNTIME_NETWORK_APPLE_H
#define RAE_RUNTIME_NETWORK_APPLE_H

#include <stdint.h>

#if defined(__APPLE__) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <Network/Network.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

void rae_ext_EventLoop_wake(void);

/* The event kinds, as lib/net/NetworkConnection names them */
#define RAE_NW_READY 1
#define RAE_NW_MESSAGE 2
#define RAE_NW_CLOSED 3
#define RAE_NW_FAILED 4
#define RAE_NW_PATH 5
#define RAE_NW_WAITING 6
#define RAE_NW_SEND_FAILED 7

/* The connection modes */
#define RAE_NW_MODE_TCP 0
#define RAE_NW_MODE_TLS 1
#define RAE_NW_MODE_WEBSOCKET 2

typedef struct RaeNwEvent {
  int64_t kind;
  int64_t code;
  uint8_t* data;
  int64_t length;
  struct RaeNwEvent* next;
} RaeNwEvent;

typedef struct RaeNwConnection {
  nw_connection_t connection;
  dispatch_queue_t queue;
  pthread_mutex_t lock;
  pthread_cond_t arrived;
  RaeNwEvent* head;
  RaeNwEvent* tail;
  RaeNwEvent* current;
  int webSocket;
  /* the framework will call no handler any more (cancelled) */
  int finished;
  /* Rae has let go of the handle */
  int released;
} RaeNwConnection;

static void rae_nw_free_event(RaeNwEvent* event) {
  if (!event) return;
  free(event->data);
  free(event);
}

static void rae_nw_push(RaeNwConnection* c, int64_t kind, int64_t code, const void* data, size_t length) {
  RaeNwEvent* event = (RaeNwEvent*)calloc(1, sizeof *event);
  if (!event) return;
  event->kind = kind;
  event->code = code;
  if (length > 0) {
    event->data = (uint8_t*)malloc(length);
    if (event->data) {
      memcpy(event->data, data, length);
      event->length = (int64_t)length;
    }
  }
  pthread_mutex_lock(&c->lock);
  if (c->tail) c->tail->next = event; else c->head = event;
  c->tail = event;
  pthread_cond_broadcast(&c->arrived);
  pthread_mutex_unlock(&c->lock);
  rae_ext_EventLoop_wake();
}

/* An error as an event: its code, and its description as the bytes */
static void rae_nw_push_error(RaeNwConnection* c, int64_t kind, nw_error_t error) {
  int64_t code = error ? nw_error_get_error_code(error) : 0;
  char text[512] = "";
  if (error) {
    CFErrorRef cfError = nw_error_copy_cf_error(error);
    if (cfError) {
      CFStringRef description = CFErrorCopyDescription(cfError);
      if (description) {
        CFStringGetCString(description, text, sizeof text, kCFStringEncodingUTF8);
        CFRelease(description);
      }
      CFRelease(cfError);
    }
  }
  rae_nw_push(c, kind, code, text, strlen(text));
}

/* The bytes of a dispatch_data as one event */
static void rae_nw_push_data(RaeNwConnection* c, int64_t kind, int64_t code, dispatch_data_t content) {
  const void* buffer = NULL;
  size_t size = 0;
  dispatch_data_t map = dispatch_data_create_map(content, &buffer, &size);
  rae_nw_push(c, kind, code, buffer, size);
  if (map) dispatch_release(map);
}

/* Free the connection once the framework is finished with it and Rae has let
 * go; runs on the connection's queue */
static void rae_nw_maybe_free(RaeNwConnection* c) {
  if (!c->finished || !c->released) return;
  nw_release(c->connection);
  RaeNwEvent* event = c->head;
  while (event) {
    RaeNwEvent* next = event->next;
    rae_nw_free_event(event);
    event = next;
  }
  rae_nw_free_event(c->current);
  pthread_mutex_destroy(&c->lock);
  pthread_cond_destroy(&c->arrived);
  dispatch_queue_t queue = c->queue;
  free(c);
  dispatch_release(queue);
}

static void rae_nw_receive(RaeNwConnection* c);

static void rae_nw_receive_stream(RaeNwConnection* c) {
  nw_connection_receive(c->connection, 1, 65536,
                        ^(dispatch_data_t content, nw_content_context_t context, bool is_complete, nw_error_t error) {
    if (content) rae_nw_push_data(c, RAE_NW_MESSAGE, 0, content);
    if (error) {
      if (nw_error_get_error_code(error) != 89 /* ECANCELED */) rae_nw_push_error(c, RAE_NW_FAILED, error);
      return;
    }
    if (is_complete && (context == NULL || nw_content_context_get_is_final(context))) {
      rae_nw_push(c, RAE_NW_CLOSED, 0, NULL, 0);
      return;
    }
    rae_nw_receive(c);
  });
}

static void rae_nw_receive_message(RaeNwConnection* c) {
  nw_connection_receive_message(c->connection,
                                ^(dispatch_data_t content, nw_content_context_t context, bool is_complete, nw_error_t error) {
    (void)is_complete;
    int64_t opcode = 0;
    int64_t closeCode = 0;
    if (context) {
      nw_protocol_definition_t definition = nw_protocol_copy_ws_definition();
      nw_protocol_metadata_t metadata = nw_content_context_copy_protocol_metadata(context, definition);
      if (metadata) {
        opcode = (int64_t)nw_ws_metadata_get_opcode(metadata);
        closeCode = (int64_t)nw_ws_metadata_get_close_code(metadata);
        nw_release(metadata);
      }
      nw_release(definition);
    }
    if (opcode == nw_ws_opcode_close) {
      rae_nw_push(c, RAE_NW_CLOSED, closeCode, NULL, 0);
      return;
    }
    if (content) rae_nw_push_data(c, RAE_NW_MESSAGE, opcode, content);
    if (error) {
      if (nw_error_get_error_code(error) != 89 /* ECANCELED */) rae_nw_push_error(c, RAE_NW_FAILED, error);
      return;
    }
    rae_nw_receive(c);
  });
}

static void rae_nw_receive(RaeNwConnection* c) {
  if (c->webSocket) rae_nw_receive_message(c); else rae_nw_receive_stream(c);
}

/* Open a connection: to `hostZ`:`port` (TCP or TLS), or to the ws:// / wss://
 * `urlZ` (a WebSocket client). `insecure` accepts any server certificate
 * (only for a test against a self-signed server). The handle, or 0. */
int64_t rae_ext_Nw_connect(uint8_t* hostZ, int64_t port, uint8_t* urlZ, int64_t mode, int64_t insecure) {
  RaeNwConnection* c = (RaeNwConnection*)calloc(1, sizeof *c);
  if (!c) return 0;
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->arrived, NULL);
  c->webSocket = mode == RAE_NW_MODE_WEBSOCKET;
  c->queue = dispatch_queue_create("rae.network.connection", DISPATCH_QUEUE_SERIAL);

  int tls = mode == RAE_NW_MODE_TLS
            || (mode == RAE_NW_MODE_WEBSOCKET && strncmp((const char*)urlZ, "wss:", 4) == 0);
  dispatch_queue_t queue = c->queue;
  nw_parameters_configure_protocol_block_t configureTls = NW_PARAMETERS_DISABLE_PROTOCOL;
  if (tls) {
    configureTls = ^(nw_protocol_options_t tlsOptions) {
      if (insecure) {
        sec_protocol_options_t security = nw_tls_copy_sec_protocol_options(tlsOptions);
        sec_protocol_options_set_verify_block(security,
          ^(sec_protocol_metadata_t metadata, sec_trust_t trust, sec_protocol_verify_complete_t complete) {
            (void)metadata; (void)trust;
            complete(true);
          }, queue);
        nw_release(security);
      }
    };
  }
  nw_parameters_t parameters = nw_parameters_create_secure_tcp(configureTls, NW_PARAMETERS_DEFAULT_CONFIGURATION);
  nw_endpoint_t endpoint = NULL;
  if (c->webSocket) {
    nw_protocol_options_t webSocketOptions = nw_ws_create_options(nw_ws_version_13);
    nw_ws_options_set_auto_reply_ping(webSocketOptions, true);
    nw_protocol_stack_t stack = nw_parameters_copy_default_protocol_stack(parameters);
    nw_protocol_stack_prepend_application_protocol(stack, webSocketOptions);
    nw_release(stack);
    nw_release(webSocketOptions);
    endpoint = nw_endpoint_create_url((const char*)urlZ);
  } else {
    char portText[16];
    snprintf(portText, sizeof portText, "%lld", (long long)port);
    endpoint = nw_endpoint_create_host((const char*)hostZ, portText);
  }
  if (!endpoint) {
    nw_release(parameters);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->arrived);
    dispatch_release(c->queue);
    free(c);
    return 0;
  }
  c->connection = nw_connection_create(endpoint, parameters);
  nw_release(endpoint);
  nw_release(parameters);

  nw_connection_set_queue(c->connection, c->queue);
  nw_connection_set_state_changed_handler(c->connection, ^(nw_connection_state_t state, nw_error_t error) {
    switch (state) {
      case nw_connection_state_ready:
        rae_nw_push(c, RAE_NW_READY, 0, NULL, 0);
        rae_nw_receive(c);
        break;
      case nw_connection_state_waiting:
        rae_nw_push_error(c, RAE_NW_WAITING, error);
        break;
      case nw_connection_state_failed:
        rae_nw_push_error(c, RAE_NW_FAILED, error);
        break;
      case nw_connection_state_cancelled:
        c->finished = 1;
        rae_nw_maybe_free(c);
        break;
      default:
        break;
    }
  });
  nw_connection_set_path_changed_handler(c->connection, ^(nw_path_t path) {
    const char* interface = "other";
    if (nw_path_uses_interface_type(path, nw_interface_type_wifi)) interface = "wifi";
    else if (nw_path_uses_interface_type(path, nw_interface_type_cellular)) interface = "cellular";
    else if (nw_path_uses_interface_type(path, nw_interface_type_wired)) interface = "wired";
    else if (nw_path_uses_interface_type(path, nw_interface_type_loopback)) interface = "loopback";
    rae_nw_push(c, RAE_NW_PATH, (int64_t)nw_path_get_status(path), interface, strlen(interface));
  });
  nw_connection_start(c->connection);
  return (int64_t)(intptr_t)c;
}

/* Send `count` bytes of `bytes` from `offset`. On a WebSocket `opcode` says
 * the message's kind (1 text, 2 binary, 8 close with `closeCode`, 9 ping);
 * elsewhere it is ignored. A failure arrives as a send-failed event. */
int64_t rae_ext_Nw_send(int64_t handle, uint8_t* bytes, int64_t offset, int64_t count, int64_t opcode, int64_t closeCode) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  if (!c) return -1;
  dispatch_data_t data = dispatch_data_create(count > 0 ? (const void*)(bytes + offset) : (const void*)"", (size_t)(count > 0 ? count : 0),
                                              c->queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  nw_content_context_t context = NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT;
  if (c->webSocket) {
    nw_protocol_metadata_t metadata = nw_ws_create_metadata((nw_ws_opcode_t)opcode);
    if (opcode == nw_ws_opcode_close) nw_ws_metadata_set_close_code(metadata, (nw_ws_close_code_t)closeCode);
    context = nw_content_context_create("rae.send");
    nw_content_context_set_metadata_for_protocol(context, metadata);
    nw_release(metadata);
  }
  nw_connection_send(c->connection, data, context, true, ^(nw_error_t error) {
    if (error) rae_nw_push_error(c, RAE_NW_SEND_FAILED, error);
  });
  if (c->webSocket) nw_release(context);
  dispatch_release(data);
  return 0;
}

/* Take the next event, waiting up to `timeoutMs` for one (0: do not wait):
 * its kind, or 0 when there is none. The event's code, length and bytes are
 * read with the three calls below until the next call. */
int64_t rae_ext_Nw_nextEvent(int64_t handle, int64_t timeoutMs) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  if (!c) return 0;
  pthread_mutex_lock(&c->lock);
  rae_nw_free_event(c->current);
  c->current = NULL;
  if (!c->head && timeoutMs > 0) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec deadline;
    int64_t nanoseconds = (int64_t)now.tv_usec * 1000 + (timeoutMs % 1000) * 1000000;
    deadline.tv_sec = now.tv_sec + (time_t)(timeoutMs / 1000) + (time_t)(nanoseconds / 1000000000);
    deadline.tv_nsec = (long)(nanoseconds % 1000000000);
    while (!c->head) {
      if (pthread_cond_timedwait(&c->arrived, &c->lock, &deadline) != 0) break;
    }
  }
  int64_t kind = 0;
  if (c->head) {
    c->current = c->head;
    c->head = c->head->next;
    if (!c->head) c->tail = NULL;
    kind = c->current->kind;
  }
  pthread_mutex_unlock(&c->lock);
  return kind;
}

int64_t rae_ext_Nw_eventCode(int64_t handle) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  return c && c->current ? c->current->code : 0;
}

int64_t rae_ext_Nw_eventLength(int64_t handle) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  return c && c->current ? c->current->length : 0;
}

void rae_ext_Nw_eventCopy(int64_t handle, uint8_t* into) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  if (c && c->current && c->current->length > 0) memcpy(into, c->current->data, (size_t)c->current->length);
}

/* Close the connection and let go of the handle (do not use it after) */
void rae_ext_Nw_release(int64_t handle) {
  RaeNwConnection* c = (RaeNwConnection*)(intptr_t)handle;
  if (!c) return;
  dispatch_async(c->queue, ^{
    c->released = 1;
    nw_connection_cancel(c->connection);
    rae_nw_maybe_free(c);
  });
}
#endif

#endif
