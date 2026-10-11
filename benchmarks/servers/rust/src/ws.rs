// The WebSocket layer the tuned Rust references share (spec/WebSocket.md
// "The connection"), written directly on tokio's TcpStream for a single-thread
// runtime:
//
// - accept: the upgrade request parsed with httparse, the accept key from
//   tungstenite's derive_accept_key; any other request gets a 404.
// - Output is corked, as uWebSockets and Rae's servers do it: send() only
//   queues a frame (shared bytes, encoded once) and marks the connection
//   dirty on the Hub. The Hub's task runs after the current batch of ready
//   tasks and writes each dirty connection once, every queued frame in one
//   writev. Several broadcasts in one pass cost one write per client, with no
//   channel and no task wake-up per client. A socket the kernel will not take
//   more from is finished by that connection's own task when it is writable.
// - FrameReader: masked client frames, fragments, ping -> pong, close -> close.

// Each binary uses part of it
#![allow(dead_code)]

use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use std::io::{ErrorKind, IoSlice};
use std::rc::Rc;

use bytes::{Bytes, BytesMut};
use tokio::net::TcpStream;
use tokio::sync::Notify;
use tokio_tungstenite::tungstenite::handshake::derive_accept_key;

pub const TEXT: u8 = 1;
pub const BINARY: u8 = 2;
const CONTINUATION: u8 = 0;
const CLOSE: u8 = 8;
const PING: u8 = 9;
const PONG: u8 = 10;

pub struct Connection {
    stream: TcpStream,
    hub: Rc<Hub>,
    // Frames (or their unwritten tails) the socket has not taken yet
    outbox: RefCell<VecDeque<Bytes>>,
    queued: Cell<usize>,
    closed: Cell<bool>,
    // On the Hub's dirty list
    dirty: Cell<bool>,
    // The socket refused bytes: this connection's task waits for writable
    blocked: Cell<bool>,
    wake_writer: Notify,
}

/// The connections with queued output, written together after each batch
#[derive(Default)]
pub struct Hub {
    dirty: RefCell<Vec<Rc<Connection>>>,
    wake: Notify,
}

impl Hub {
    pub fn new() -> Rc<Hub> {
        let hub = Rc::new(Hub::default());
        tokio::task::spawn_local(hub.clone().run());
        hub
    }

    async fn run(self: Rc<Self>) {
        loop {
            self.wake.notified().await;
            self.flush();
        }
    }

    /// Writes every dirty connection now
    pub fn flush(&self) {
        let dirty = std::mem::take(&mut *self.dirty.borrow_mut());
        for connection in dirty {
            connection.dirty.set(false);
            if !connection.blocked.get() {
                connection.write_now();
            }
        }
    }
}

/// The server's frame for `payload`: FIN, no mask
pub fn frame(opcode: u8, payload: &[u8]) -> Bytes {
    let mut out = BytesMut::with_capacity(payload.len() + 10);
    out.extend_from_slice(&[0x80 | opcode]);
    if payload.len() < 126 {
        out.extend_from_slice(&[payload.len() as u8]);
    } else if payload.len() <= 0xffff {
        out.extend_from_slice(&[126]);
        out.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    } else {
        out.extend_from_slice(&[127]);
        out.extend_from_slice(&(payload.len() as u64).to_be_bytes());
    }
    out.extend_from_slice(payload);
    out.freeze()
}

impl Connection {
    /// Bytes handed to send() that the socket has not taken yet
    pub fn queued(&self) -> usize {
        self.queued.get()
    }

    pub fn send(self: &Rc<Self>, frame: &Bytes) {
        if self.closed.get() {
            return;
        }
        self.queued.set(self.queued.get() + frame.len());
        self.outbox.borrow_mut().push_back(frame.clone());
        if !self.dirty.get() {
            self.dirty.set(true);
            self.hub.dirty.borrow_mut().push(self.clone());
            self.hub.wake.notify_one();
        }
    }

    pub fn close(&self) {
        self.closed.set(true);
        self.wake_writer.notify_one();
    }

    /// Writes what is still queued (a close frame's answer), then closes
    pub async fn finish(&self) {
        self.write_now();
        while !self.closed.get() && !self.outbox.borrow().is_empty() {
            if self.stream.writable().await.is_err() {
                break;
            }
            self.write_now();
        }
        self.close();
    }

    // Writes the outbox until it is empty or the socket is full (then the
    // connection's writer task takes over)
    fn write_now(&self) {
        while !self.closed.get() && !self.outbox.borrow().is_empty() {
            let result = {
                let outbox = self.outbox.borrow();
                let slices: Vec<IoSlice> = outbox.iter().take(64).map(|chunk| IoSlice::new(chunk)).collect();
                self.stream.try_write_vectored(&slices)
            };
            match result {
                Ok(mut count) => {
                    self.queued.set(self.queued.get() - count);
                    let mut outbox = self.outbox.borrow_mut();
                    while count > 0 {
                        let front = outbox.front_mut().unwrap();
                        if count >= front.len() {
                            count -= front.len();
                            outbox.pop_front();
                        } else {
                            *front = front.slice(count..);
                            count = 0;
                        }
                    }
                }
                Err(error) if error.kind() == ErrorKind::WouldBlock => {
                    self.blocked.set(true);
                    self.wake_writer.notify_one();
                    return;
                }
                Err(_) => {
                    self.close();
                    return;
                }
            }
        }
    }

    // Finishes the writes of a socket that was full, until the connection closes
    async fn write_when_blocked(self: Rc<Self>) {
        loop {
            self.wake_writer.notified().await;
            while self.blocked.get() && !self.closed.get() {
                if self.stream.writable().await.is_err() {
                    self.close();
                }
                self.blocked.set(false);
                self.write_now();
            }
            if self.closed.get() {
                return;
            }
        }
    }
}

/// Reads the upgrade request and answers it: the connection (its writer task
/// started on the LocalSet) and a reader holding any bytes read past the
/// request, or None when it was not a WebSocket upgrade on /ws
pub async fn accept(stream: TcpStream, hub: &Rc<Hub>) -> Option<(Rc<Connection>, FrameReader)> {
    let _ = stream.set_nodelay(true);
    let mut buffer = BytesMut::with_capacity(4096);
    let (header_length, key) = loop {
        stream.readable().await.ok()?;
        match stream.try_read_buf(&mut buffer) {
            Ok(0) => return None,
            Ok(_) => {}
            Err(error) if error.kind() == ErrorKind::WouldBlock => continue,
            Err(_) => return None,
        }
        let mut headers = [httparse::EMPTY_HEADER; 32];
        let mut request = httparse::Request::new(&mut headers);
        match request.parse(&buffer) {
            Ok(httparse::Status::Complete(length)) => {
                let key = request
                    .headers
                    .iter()
                    .find(|header| header.name.eq_ignore_ascii_case("sec-websocket-key"))
                    .map(|header| header.value.to_vec());
                match (request.path, key) {
                    (Some("/ws"), Some(key)) => break (length, key),
                    _ => {
                        let _ = write_all(&stream, b"HTTP/1.1 404 Not Found\r\nServer: rust\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found").await;
                        return None;
                    }
                }
            }
            Ok(httparse::Status::Partial) if buffer.len() < 65536 => {}
            _ => return None,
        }
    };
    let answer = format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
        derive_accept_key(&key)
    );
    write_all(&stream, answer.as_bytes()).await.ok()?;
    let _ = buffer.split_to(header_length);
    let connection = Rc::new(Connection {
        stream,
        hub: hub.clone(),
        outbox: RefCell::new(VecDeque::new()),
        queued: Cell::new(0),
        closed: Cell::new(false),
        dirty: Cell::new(false),
        blocked: Cell::new(false),
        wake_writer: Notify::new(),
    });
    tokio::task::spawn_local(connection.clone().write_when_blocked());
    Some((connection, FrameReader { buffer, message: Vec::new(), message_opcode: 0 }))
}

async fn write_all(stream: &TcpStream, mut bytes: &[u8]) -> std::io::Result<()> {
    while !bytes.is_empty() {
        stream.writable().await?;
        match stream.try_write(bytes) {
            Ok(count) => bytes = &bytes[count..],
            Err(error) if error.kind() == ErrorKind::WouldBlock => {}
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

pub struct FrameReader {
    buffer: BytesMut,
    // The fragments of a message still being received
    message: Vec<u8>,
    message_opcode: u8,
}

impl FrameReader {
    /// The next whole data message (its opcode and payload), answering pings
    /// and closes on the way; None when the connection ends
    pub async fn next(&mut self, connection: &Rc<Connection>) -> Option<(u8, Vec<u8>)> {
        loop {
            while let Some((fin, opcode, payload)) = self.take_frame() {
                match opcode {
                    PING => connection.send(&frame(PONG, &payload)),
                    PONG => {}
                    CLOSE => {
                        connection.send(&frame(CLOSE, &payload[..payload.len().min(2)]));
                        return None;
                    }
                    CONTINUATION => {
                        self.message.extend_from_slice(&payload);
                        if fin {
                            return Some((self.message_opcode, std::mem::take(&mut self.message)));
                        }
                    }
                    _ if fin => return Some((opcode, payload)),
                    _ => {
                        self.message_opcode = opcode;
                        self.message = payload;
                    }
                }
            }
            if connection.closed.get() || connection.stream.readable().await.is_err() {
                return None;
            }
            self.buffer.reserve(65536);
            match connection.stream.try_read_buf(&mut self.buffer) {
                Ok(0) => return None,
                Ok(_) => {}
                Err(error) if error.kind() == ErrorKind::WouldBlock => {}
                Err(_) => return None,
            }
        }
    }

    // One whole frame from the buffer, unmasked: (fin, opcode, payload)
    fn take_frame(&mut self) -> Option<(bool, u8, Vec<u8>)> {
        let bytes = &self.buffer[..];
        if bytes.len() < 2 {
            return None;
        }
        let fin = bytes[0] & 0x80 != 0;
        let opcode = bytes[0] & 0x0f;
        let masked = bytes[1] & 0x80 != 0;
        let (length, mut at) = match bytes[1] & 0x7f {
            126 if bytes.len() >= 4 => (u16::from_be_bytes([bytes[2], bytes[3]]) as usize, 4),
            127 if bytes.len() >= 10 => (u64::from_be_bytes(bytes[2..10].try_into().unwrap()) as usize, 10),
            126 | 127 => return None,
            length => (length as usize, 2),
        };
        let mask = if masked {
            if bytes.len() < at + 4 {
                return None;
            }
            at += 4;
            Some([bytes[at - 4], bytes[at - 3], bytes[at - 2], bytes[at - 1]])
        } else {
            None
        };
        if bytes.len() < at + length {
            return None;
        }
        let mut payload = bytes[at..at + length].to_vec();
        if let Some(mask) = mask {
            for (i, byte) in payload.iter_mut().enumerate() {
                *byte ^= mask[i & 3];
            }
        }
        let _ = self.buffer.split_to(at + length);
        Some((fin, opcode, payload))
    }
}
