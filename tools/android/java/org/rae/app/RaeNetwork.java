// One client connection for lib/net/NetworkConnection on Android
// (docs/network-connections.md; compiler/runtime/runtime_network_android.h is the
// other half): plain TCP, TLS checked against the system trust store, or a WebSocket
// client (ws:// or wss://).
//
// Platform reason: the NDK has no TLS and no access to the system's trust store; only
// Java has them, and Android has no public WebSocket client to go with them, so its
// frames are here too. Two threads per connection: one connects and reads, one writes.
// Every event goes to the C side's queue, and Rae decides what it means. As on Apple,
// a connection that cannot be made reports waiting and tries again until closed, and a
// certificate that fails is the end of it.
package org.rae.app;

import android.util.Base64;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.Locale;
import java.util.concurrent.LinkedBlockingQueue;

import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

public final class RaeNetwork {
  // The event kinds and modes, as in runtime_network_android.h
  private static final int READY = 1;
  private static final int MESSAGE = 2;
  private static final int CLOSED = 3;
  private static final int FAILED = 4;
  private static final int PATH = 5;
  private static final int WAITING = 6;
  private static final int SEND_FAILED = 7;
  private static final int MODE_TLS = 1;
  private static final int MODE_WEBSOCKET = 2;

  private static final int OPCODE_CONTINUATION = 0;
  private static final int OPCODE_CLOSE = 8;
  private static final int OPCODE_PING = 9;
  private static final int OPCODE_PONG = 10;

  private static final int CONNECT_TIMEOUT_MS = 10000;
  private static final int RETRY_EVERY_MS = 1000;
  private static final int LARGEST_MESSAGE = 64 * 1024 * 1024;
  private static final String WEB_SOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

  private static native void nativeEvent(long handle, int kind, int code, byte[] data);

  private static native void nativeFinished(long handle);

  private static final class Outgoing {
    final byte[] bytes;
    final int opcode;
    final int closeCode;
    final boolean stop;

    Outgoing(byte[] bytes, int opcode, int closeCode, boolean stop) {
      this.bytes = bytes;
      this.opcode = opcode;
      this.closeCode = closeCode;
      this.stop = stop;
    }
  }

  private final long handle;
  private final int mode;
  private final String host;
  private final int port;
  private final String path;
  private final boolean secure;
  private final boolean insecure;
  private final LinkedBlockingQueue<Outgoing> outgoing = new LinkedBlockingQueue<>();
  private final SecureRandom random = new SecureRandom();
  private volatile boolean closed;
  private volatile Socket socket;
  private Thread reader;

  // The handle is the C side's connection; null when the URL cannot be read
  public static RaeNetwork connect(
      long handle, String host, int port, String url, int mode, boolean insecure) {
    try {
      RaeNetwork connection = new RaeNetwork(handle, host, port, url, mode, insecure);
      connection.reader = new Thread(connection::run, "rae-network");
      connection.reader.setDaemon(true);
      connection.reader.start();
      return connection;
    } catch (IllegalArgumentException | java.net.URISyntaxException error) {
      return null;
    }
  }

  private RaeNetwork(long handle, String host, int port, String url, int mode, boolean insecure)
      throws java.net.URISyntaxException {
    this.handle = handle;
    this.mode = mode;
    this.insecure = insecure;
    if (mode == MODE_WEBSOCKET) {
      URI uri = new URI(url);
      String scheme = uri.getScheme();
      if (uri.getHost() == null || !("ws".equals(scheme) || "wss".equals(scheme))) {
        throw new IllegalArgumentException("not a ws:// or wss:// URL");
      }
      this.secure = "wss".equals(scheme);
      this.host = uri.getHost();
      this.port = uri.getPort() > 0 ? uri.getPort() : (this.secure ? 443 : 80);
      String rawPath = uri.getRawPath() == null || uri.getRawPath().isEmpty() ? "/" : uri.getRawPath();
      this.path = uri.getRawQuery() == null ? rawPath : rawPath + "?" + uri.getRawQuery();
    } else {
      this.secure = mode == MODE_TLS;
      this.host = host;
      this.port = port;
      this.path = "";
    }
  }

  // From the thread Rae runs on: never blocks
  public void send(byte[] bytes, int opcode, int closeCode) {
    outgoing.add(new Outgoing(bytes, opcode, closeCode, false));
  }

  // From the thread Rae runs on: the connection ends with no more events
  public void close() {
    closed = true;
    outgoing.add(new Outgoing(null, 0, 0, true));
    Socket current = socket;
    if (current != null) {
      new Thread(() -> closeQuietly(current), "rae-network-close").start();
    }
    if (reader != null) {
      reader.interrupt();
    }
  }

  private void event(int kind, int code, byte[] data) {
    if (!closed) {
      nativeEvent(handle, kind, code, data);
    }
  }

  private void eventText(int kind, String text) {
    event(kind, 0, text == null ? new byte[0] : text.getBytes(StandardCharsets.UTF_8));
  }

  private static void closeQuietly(Socket target) {
    try {
      target.close();
    } catch (IOException error) {
      // Going away anyway
    }
  }

  private void run() {
    Thread writer = null;
    try {
      Socket connected = connectUntilClosed();
      if (connected == null) {
        return;
      }
      socket = connected;
      InputStream input = connected.getInputStream();
      OutputStream output = connected.getOutputStream();
      if (mode == MODE_WEBSOCKET) {
        webSocketHandshake(input, output);
      }
      // There is one network here to speak of, and it just worked. Android's own
      // changes of network are not followed yet (docs/network-connections.md).
      event(PATH, 1, "other".getBytes(StandardCharsets.UTF_8));
      event(READY, 0, new byte[0]);
      final OutputStream writing = output;
      writer = new Thread(() -> writeLoop(writing), "rae-network-write");
      writer.setDaemon(true);
      writer.start();
      if (mode == MODE_WEBSOCKET) {
        readFrames(input, output);
      } else {
        readStream(input);
      }
    } catch (IOException | RuntimeException error) {
      eventText(FAILED, describe(error));
    } finally {
      Socket current = socket;
      if (current != null) {
        closeQuietly(current);
      }
      outgoing.add(new Outgoing(null, 0, 0, true));
      if (writer != null) {
        try {
          writer.join();
        } catch (InterruptedException error) {
          Thread.currentThread().interrupt();
        }
      }
      nativeFinished(handle);
    }
  }

  private static String describe(Throwable error) {
    String message = error.getMessage();
    return message == null || message.isEmpty() ? error.getClass().getSimpleName() : message;
  }

  // A socket, connected (and its TLS done); null when closed first
  private Socket connectUntilClosed() throws IOException {
    boolean told = false;
    while (!closed) {
      Socket attempt = null;
      try {
        attempt = openSocket();
        return attempt;
      } catch (SSLException error) {
        // A certificate does not get better by trying again
        if (attempt != null) {
          closeQuietly(attempt);
        }
        throw error;
      } catch (IOException error) {
        if (!told) {
          told = true;
          eventText(WAITING, describe(error));
        }
      }
      try {
        Thread.sleep(RETRY_EVERY_MS);
      } catch (InterruptedException error) {
        return null;
      }
    }
    return null;
  }

  private Socket openSocket() throws IOException {
    Socket plain = new Socket();
    try {
      plain.setTcpNoDelay(true);
      plain.connect(new InetSocketAddress(host, port), CONNECT_TIMEOUT_MS);
      if (!secure) {
        return plain;
      }
      SSLSocket tls = (SSLSocket) tlsFactory().createSocket(plain, host, port, true);
      if (!insecure) {
        // The name in the certificate must be this host's: the factory alone checks
        // only that a trusted authority signed it
        SSLParameters parameters = tls.getSSLParameters();
        parameters.setEndpointIdentificationAlgorithm("HTTPS");
        tls.setSSLParameters(parameters);
      }
      tls.startHandshake();
      return tls;
    } catch (IOException | RuntimeException error) {
      closeQuietly(plain);
      throw error;
    }
  }

  private SSLSocketFactory tlsFactory() throws IOException {
    if (!insecure) {
      return (SSLSocketFactory) SSLSocketFactory.getDefault();
    }
    // networkConnectTlsInsecure: only for a test against a self-signed server
    try {
      SSLContext context = SSLContext.getInstance("TLS");
      TrustManager acceptAll = new X509TrustManager() {
        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType) {
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType) {
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() {
          return new X509Certificate[0];
        }
      };
      context.init(null, new TrustManager[] {acceptAll}, random);
      return context.getSocketFactory();
    } catch (java.security.GeneralSecurityException error) {
      throw new IOException(describe(error));
    }
  }

  private void readStream(InputStream input) throws IOException {
    byte[] buffer = new byte[16384];
    while (!closed) {
      int count = input.read(buffer);
      if (count < 0) {
        event(CLOSED, 0, new byte[0]);
        return;
      }
      if (count > 0) {
        byte[] data = new byte[count];
        System.arraycopy(buffer, 0, data, 0, count);
        event(MESSAGE, 0, data);
      }
    }
  }

  private void writeLoop(OutputStream output) {
    while (true) {
      Outgoing next;
      try {
        next = outgoing.take();
      } catch (InterruptedException error) {
        return;
      }
      if (next.stop) {
        return;
      }
      try {
        if (mode == MODE_WEBSOCKET) {
          byte[] payload = next.bytes;
          if (next.opcode == OPCODE_CLOSE) {
            payload = new byte[] {(byte) (next.closeCode >> 8), (byte) next.closeCode};
          }
          writeFrame(output, next.opcode, payload);
        } else {
          output.write(next.bytes);
          output.flush();
        }
      } catch (IOException error) {
        eventText(SEND_FAILED, describe(error));
      }
    }
  }

  // ----- the WebSocket protocol (RFC 6455) --------------------------------------
  private void webSocketHandshake(InputStream input, OutputStream output) throws IOException {
    byte[] nonce = new byte[16];
    random.nextBytes(nonce);
    String key = Base64.encodeToString(nonce, Base64.NO_WRAP);
    boolean standardPort = port == (secure ? 443 : 80);
    String request = "GET " + path + " HTTP/1.1\r\n"
        + "Host: " + host + (standardPort ? "" : ":" + port) + "\r\n"
        + "Upgrade: websocket\r\n"
        + "Connection: Upgrade\r\n"
        + "Sec-WebSocket-Key: " + key + "\r\n"
        + "Sec-WebSocket-Version: 13\r\n"
        + "\r\n";
    output.write(request.getBytes(StandardCharsets.US_ASCII));
    output.flush();
    String response = readHeaders(input);
    String[] lines = response.split("\r\n");
    if (lines.length == 0 || !lines[0].matches("HTTP/1\\.1 101( .*)?")) {
      throw new IOException("the server did not accept the WebSocket: "
          + (lines.length == 0 ? "" : lines[0]));
    }
    String expected = acceptOf(key);
    for (String line : lines) {
      int colon = line.indexOf(':');
      if (colon > 0 && line.substring(0, colon).trim().toLowerCase(Locale.ROOT)
          .equals("sec-websocket-accept")
          && line.substring(colon + 1).trim().equals(expected)) {
        return;
      }
    }
    throw new IOException("the server's WebSocket answer has no good Sec-WebSocket-Accept");
  }

  private static String acceptOf(String key) throws IOException {
    try {
      MessageDigest digest = MessageDigest.getInstance("SHA-1");
      byte[] hash = digest.digest((key + WEB_SOCKET_GUID).getBytes(StandardCharsets.US_ASCII));
      return Base64.encodeToString(hash, Base64.NO_WRAP);
    } catch (java.security.NoSuchAlgorithmException error) {
      throw new IOException(describe(error));
    }
  }

  // One byte at a time: what follows the headers is the first frame, and stays unread
  private static String readHeaders(InputStream input) throws IOException {
    ByteArrayOutputStream headers = new ByteArrayOutputStream();
    int matched = 0;
    final byte[] end = {'\r', '\n', '\r', '\n'};
    while (matched < end.length) {
      int value = input.read();
      if (value < 0) {
        throw new EOFException("the server closed during the WebSocket handshake");
      }
      if (headers.size() > 65536) {
        throw new IOException("the server's WebSocket answer is too long");
      }
      headers.write(value);
      matched = value == end[matched] ? matched + 1 : (value == end[0] ? 1 : 0);
    }
    return new String(headers.toByteArray(), StandardCharsets.ISO_8859_1);
  }

  private static byte[] readFully(InputStream input, int count) throws IOException {
    byte[] data = new byte[count];
    int done = 0;
    while (done < count) {
      int got = input.read(data, done, count - done);
      if (got < 0) {
        throw new EOFException("the server closed in the middle of a WebSocket frame");
      }
      done += got;
    }
    return data;
  }

  private void readFrames(InputStream input, OutputStream output) throws IOException {
    ByteArrayOutputStream message = new ByteArrayOutputStream();
    int messageOpcode = 0;
    while (!closed) {
      byte[] head = readFully(input, 2);
      boolean last = (head[0] & 0x80) != 0;
      int opcode = head[0] & 0x0f;
      boolean masked = (head[1] & 0x80) != 0;
      long length = head[1] & 0x7f;
      if (length == 126) {
        byte[] extended = readFully(input, 2);
        length = ((extended[0] & 0xffL) << 8) | (extended[1] & 0xffL);
      } else if (length == 127) {
        byte[] extended = readFully(input, 8);
        length = 0;
        for (byte part : extended) {
          length = (length << 8) | (part & 0xffL);
        }
      }
      if (length < 0 || length + message.size() > LARGEST_MESSAGE) {
        throw new IOException("a WebSocket message is too long");
      }
      byte[] mask = masked ? readFully(input, 4) : null;
      byte[] payload = readFully(input, (int) length);
      if (masked) {
        for (int i = 0; i < payload.length; i++) {
          payload[i] ^= mask[i % 4];
        }
      }
      if (opcode == OPCODE_PING) {
        send(payload, OPCODE_PONG, 0);
      } else if (opcode == OPCODE_CLOSE) {
        int code = payload.length >= 2 ? ((payload[0] & 0xff) << 8) | (payload[1] & 0xff) : 1005;
        event(CLOSED, code, new byte[0]);
        return;
      } else if (opcode != OPCODE_PONG) {
        if (opcode != OPCODE_CONTINUATION) {
          messageOpcode = opcode;
        }
        message.write(payload);
        if (last) {
          event(MESSAGE, messageOpcode, message.toByteArray());
          message.reset();
        }
      }
    }
  }

  // A client's frames are masked, and one write goes out as one frame
  private void writeFrame(OutputStream output, int opcode, byte[] payload) throws IOException {
    ByteArrayOutputStream frame = new ByteArrayOutputStream(payload.length + 14);
    frame.write(0x80 | opcode);
    if (payload.length < 126) {
      frame.write(0x80 | payload.length);
    } else if (payload.length < 65536) {
      frame.write(0x80 | 126);
      frame.write(payload.length >> 8);
      frame.write(payload.length);
    } else {
      frame.write(0x80 | 127);
      for (int shift = 56; shift >= 0; shift -= 8) {
        frame.write((int) ((long) payload.length >> shift));
      }
    }
    byte[] mask = new byte[4];
    random.nextBytes(mask);
    frame.write(mask);
    for (int i = 0; i < payload.length; i++) {
      frame.write(payload[i] ^ mask[i % 4]);
    }
    output.write(frame.toByteArray());
    output.flush();
  }
}
