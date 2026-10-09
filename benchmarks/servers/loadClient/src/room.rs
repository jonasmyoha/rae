// `loadClient gameRoom` (spec/GameRoom.md "Load"):
//
//   loadClient gameRoom --url ws://127.0.0.1:8080/ws --clients 1000
//              [--seconds 10] [--warmup 1] [--threads 4] [--connect-concurrency 64]
//              [--connect-seconds 60]
//
// Connects every client, then each sends a 32-byte input every tick period
// (its own phase) and reads every snapshot. After the warmup it measures for
// `seconds`: the server's tick lateness, duration and missed ticks (from the
// headers client 0 receives), every client's snapshot inter-arrival jitter,
// the share of ticks a client did not receive, and wrong snapshots. Clients
// still not connected after `connect-seconds` are given up on (a room busy
// fanning out N² bytes accepts slowly): the window then measures the ones
// that did connect, and `connected` says how many.
//
// One JSON object on stdout; progress on stderr.
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use futures_util::stream::StreamExt;
use serde_json::{json, Value};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::MissedTickBehavior;

const PERIOD: Duration = Duration::from_nanos(1_000_000_000 / 30);

pub struct RoomSettings {
    pub url: String,
    pub clients: usize,
    pub seconds: f64,
    pub warmup: f64,
    pub threads: usize,
    pub connect_concurrency: usize,
    pub connect_seconds: f64,
}

pub fn room_settings(arguments: &[String]) -> RoomSettings {
    let mut settings = RoomSettings {
        url: "ws://127.0.0.1:8080/ws".to_string(),
        clients: 100,
        seconds: 10.0,
        warmup: 1.0,
        threads: 4,
        connect_concurrency: 64,
        connect_seconds: 60.0,
    };
    let mut i = 0;
    while i + 1 < arguments.len() {
        let value = &arguments[i + 1];
        match arguments[i].as_str() {
            "--url" => settings.url = value.clone(),
            "--clients" => settings.clients = value.parse().expect("a number"),
            "--seconds" => settings.seconds = value.parse().expect("a number"),
            "--warmup" => settings.warmup = value.parse().expect("a number"),
            "--threads" => settings.threads = value.parse().expect("a number"),
            "--connect-concurrency" => settings.connect_concurrency = value.parse().expect("a number"),
            "--connect-seconds" => settings.connect_seconds = value.parse().expect("a number"),
            other => panic!("unknown option {other}"),
        }
        i += 2;
    }
    settings
}

// One snapshot header, as client 0 saw it
#[derive(Clone, Copy)]
struct Header {
    tick: u32,
    lateness_us: u32,
    previous_tick_us: u32,
    missed: u32,
}

#[derive(Default)]
struct ClientStats {
    connected: bool,
    received: u64,
    // |interval - period| in ms between consecutive snapshots in the window
    jitter_ms: Vec<f64>,
    wrong_size: u64,
    wrong_count: u64,
    headers: Vec<Header>,
}

// The measuring window, as offsets from `base`, and the room's size in it
#[derive(Clone, Copy)]
struct Window {
    base: Instant,
    start: Duration,
    end: Duration,
    players: usize,
}

impl Window {
    fn contains(&self, at: Instant) -> bool {
        let offset = at.saturating_duration_since(self.base);
        offset >= self.start && offset < self.end
    }
}

// A busy room accepts slowly (macOS's listen backlog is 128): a failed
// connect is tried again a few times, backing off
async fn connect(address: String, path: String) -> Result<(TcpStream, Vec<u8>), String> {
    let mut failure = String::new();
    for attempt in 0..8u32 {
        match connect_once(&address, &path).await {
            Ok(connected) => return Ok(connected),
            Err(error) => failure = error,
        }
        tokio::time::sleep(Duration::from_millis(50 << attempt.min(5))).await;
    }
    Err(failure)
}

// The opening handshake by hand: the clients then read the socket's bytes
// with the frame scanner below, with no allocation per snapshot (at N = 5 000
// a tick is 400 MB; one machine cannot afford a message object for each).
// Answers the stream and any bytes that came after the 101.
async fn connect_once(address: &str, path: &str) -> Result<(TcpStream, Vec<u8>), String> {
    let mut stream = TcpStream::connect(address).await.map_err(|error| error.to_string())?;
    let _ = stream.set_nodelay(true);
    // Reset on close instead of TIME_WAIT (see main.rs)
    #[allow(deprecated)]
    let _ = stream.set_linger(Some(Duration::ZERO));
    let request = format!(
        "GET {path} HTTP/1.1\r\nHost: {address}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
         Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
    );
    stream.write_all(request.as_bytes()).await.map_err(|error| error.to_string())?;
    let mut buffer = Vec::with_capacity(512);
    let mut chunk = [0u8; 4096];
    loop {
        let read = stream.read(&mut chunk).await.map_err(|error| error.to_string())?;
        if read == 0 {
            return Err("closed during the handshake".to_string());
        }
        buffer.extend_from_slice(&chunk[..read]);
        if let Some(end) = buffer.windows(4).position(|window| window == b"\r\n\r\n") {
            if !buffer.starts_with(b"HTTP/1.1 101") {
                return Err(format!("no 101: {}", String::from_utf8_lossy(&buffer[..end.min(40)])));
            }
            return Ok((stream, buffer[end + 4..].to_vec()));
        }
    }
}

// A masked binary frame holding one 32-byte input
fn input_frame(sequence: u32, mask: [u8; 4]) -> [u8; 38] {
    let mut payload = [0u8; 32];
    payload[0..4].copy_from_slice(&sequence.to_le_bytes());
    payload[4..8].copy_from_slice(&1i32.to_le_bytes());
    payload[8..12].copy_from_slice(&(-1i32).to_le_bytes());
    let mut frame = [0u8; 38];
    frame[0] = 0x82;
    frame[1] = 0x80 | 32;
    frame[2..6].copy_from_slice(&mask);
    for (i, byte) in payload.iter().enumerate() {
        frame[6 + i] = byte ^ mask[i % 4];
    }
    frame
}

// Finds frame boundaries in the byte stream and keeps the first 24 payload
// bytes of each frame (a snapshot's header); the rest is skipped in place
struct FrameScanner {
    header: [u8; 10],
    header_length: usize,
    header_needed: usize,
    opcode: u8,
    payload_length: u64,
    payload_left: u64,
    capture: [u8; 24],
    capture_length: usize,
}

impl FrameScanner {
    fn new() -> FrameScanner {
        FrameScanner {
            header: [0; 10],
            header_length: 0,
            header_needed: 2,
            opcode: 0,
            payload_length: 0,
            payload_left: 0,
            capture: [0; 24],
            capture_length: 0,
        }
    }

    // Calls `frame(opcode, payload length, captured header bytes)` for every
    // frame that ends in `bytes`
    fn scan(&mut self, bytes: &[u8], frame: &mut impl FnMut(u8, u64, &[u8])) {
        let mut i = 0;
        while i < bytes.len() {
            if self.header_length < self.header_needed {
                self.header[self.header_length] = bytes[i];
                self.header_length += 1;
                i += 1;
                if self.header_length == 2 {
                    self.header_needed = match self.header[1] & 0x7F {
                        126 => 4,
                        127 => 10,
                        _ => 2,
                    };
                }
                if self.header_length == self.header_needed {
                    self.opcode = self.header[0] & 0x0F;
                    self.payload_length = match self.header_needed {
                        4 => u16::from_be_bytes([self.header[2], self.header[3]]) as u64,
                        10 => u64::from_be_bytes(self.header[2..10].try_into().unwrap()),
                        _ => (self.header[1] & 0x7F) as u64,
                    };
                    self.payload_left = self.payload_length;
                    self.capture_length = 0;
                    if self.payload_left == 0 {
                        self.finish(frame);
                    }
                }
                continue;
            }
            let take = (self.payload_left as usize).min(bytes.len() - i);
            let capture = take.min(24 - self.capture_length);
            self.capture[self.capture_length..self.capture_length + capture].copy_from_slice(&bytes[i..i + capture]);
            self.capture_length += capture;
            self.payload_left -= take as u64;
            i += take;
            if self.payload_left == 0 {
                self.finish(frame);
            }
        }
    }

    fn finish(&mut self, frame: &mut impl FnMut(u8, u64, &[u8])) {
        frame(self.opcode, self.payload_length, &self.capture[..self.capture_length]);
        self.header_length = 0;
        self.header_needed = 2;
    }
}

fn run_client(
    stream: TcpStream,
    leftover: Vec<u8>,
    index: usize,
    clients: usize,
    window: Arc<Mutex<Option<Window>>>,
    stats: Arc<Mutex<ClientStats>>,
) {
    stats.lock().unwrap().connected = true;
    let (mut reader, mut writer) = stream.into_split();
    // Inputs at 30 Hz, each client at its own phase
    tokio::spawn(async move {
        let phase = PERIOD.mul_f64(index as f64 / clients.max(1) as f64);
        let mut interval = tokio::time::interval_at(tokio::time::Instant::now() + phase, PERIOD);
        interval.set_missed_tick_behavior(MissedTickBehavior::Skip);
        let mask = (index as u32).wrapping_mul(2654435761).to_le_bytes();
        let mut sequence = 0u32;
        loop {
            interval.tick().await;
            sequence += 1;
            if writer.write_all(&input_frame(sequence, mask)).await.is_err() {
                break;
            }
        }
    });
    tokio::spawn(async move {
        let mut scanner = FrameScanner::new();
        let mut previous: Option<Instant> = None;
        let mut buffer = vec![0u8; 256 * 1024];
        let mut on_frame = |opcode: u8, length: u64, header: &[u8]| {
            if opcode != 2 {
                return;
            }
            let now = Instant::now();
            let Some(window) = *window.lock().unwrap() else {
                return;
            };
            if !window.contains(now) {
                previous = None;
                return;
            }
            let mut stats = stats.lock().unwrap();
            stats.received += 1;
            if let Some(before) = previous {
                let interval = now.duration_since(before).as_secs_f64() * 1000.0;
                stats.jitter_ms.push((interval - PERIOD.as_secs_f64() * 1000.0).abs());
            }
            previous = Some(now);
            if header.len() < 24 {
                stats.wrong_size += 1;
                return;
            }
            let word = |at: usize| u32::from_le_bytes(header[at..at + 4].try_into().unwrap());
            if length != 24 + 16 * word(4) as u64 {
                stats.wrong_size += 1;
                return;
            }
            if word(4) as usize != window.players {
                stats.wrong_count += 1;
            }
            if index == 0 {
                stats.headers.push(Header {
                    tick: word(0),
                    lateness_us: word(8),
                    previous_tick_us: word(12),
                    missed: word(16),
                });
            }
        };
        scanner.scan(&leftover, &mut on_frame);
        loop {
            match reader.read(&mut buffer).await {
                Ok(0) | Err(_) => break,
                Ok(read) => scanner.scan(&buffer[..read], &mut on_frame),
            }
        }
    });
}

fn spread(mut values: Vec<f64>) -> Value {
    if values.is_empty() {
        return Value::Null;
    }
    values.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let at = |fraction: f64| values[((values.len() as f64 - 1.0) * fraction).round() as usize];
    json!({"p50": at(0.5), "p99": at(0.99), "max": values[values.len() - 1]})
}

pub async fn game_room(settings: RoomSettings) -> Value {
    let without_scheme = settings.url.trim_start_matches("ws://");
    let address = without_scheme.split('/').next().expect("host:port").to_string();
    let path = format!("/{}", without_scheme.splitn(2, '/').nth(1).unwrap_or(""));
    // Each client starts sending and reading as soon as it is connected, so
    // the early ones read their snapshots while the rest connect
    let window: Arc<Mutex<Option<Window>>> = Arc::new(Mutex::new(None));
    let all_stats: Vec<Arc<Mutex<ClientStats>>> =
        (0..settings.clients).map(|_| Arc::new(Mutex::new(ClientStats::default()))).collect();
    let clients = settings.clients;
    let connected = Arc::new(AtomicUsize::new(0));
    let connecting = futures_util::stream::iter(0..clients)
        .map(|index| {
            let (address, path) = (address.clone(), path.clone());
            let (window, stats) = (window.clone(), all_stats[index].clone());
            let connected = connected.clone();
            async move {
                match connect(address, path).await {
                    Ok((stream, leftover)) => {
                        run_client(stream, leftover, index, clients, window, stats);
                        let count = connected.fetch_add(1, Ordering::Relaxed) + 1;
                        if count % 500 == 0 {
                            eprintln!("{count} clients connected");
                        }
                        None
                    }
                    Err(error) => Some(format!("connect failed at client {index}: {error}")),
                }
            }
        })
        .buffered(settings.connect_concurrency)
        .filter_map(|failure| async move { failure })
        .collect::<Vec<String>>();
    let failures = tokio::time::timeout(Duration::from_secs_f64(settings.connect_seconds), connecting)
        .await
        .unwrap_or_default();
    let connected = connected.load(Ordering::Relaxed);
    if let Some(failure) = failures.first() {
        eprintln!("{failure} ({} failed)", failures.len());
    }
    if connected < clients {
        eprintln!("{connected} of {clients} clients connected");
    }
    if connected == 0 {
        return json!({"clients": clients, "connected": 0, "error": failures.first()});
    }
    let base = Instant::now();
    let start = Duration::from_secs_f64(settings.warmup);
    let end = start + Duration::from_secs_f64(settings.seconds);
    *window.lock().unwrap() = Some(Window { base, start, end, players: connected });
    tokio::time::sleep(end + Duration::from_millis(300)).await;

    let mut jitter = Vec::new();
    let mut received = Vec::new();
    let (mut wrong_size, mut wrong_count) = (0, 0);
    for stats in all_stats.iter().filter(|stats| stats.lock().unwrap().connected) {
        let mut stats = stats.lock().unwrap();
        jitter.append(&mut stats.jitter_ms);
        received.push(stats.received as f64);
        wrong_size += stats.wrong_size;
        wrong_count += stats.wrong_count;
    }
    let headers = all_stats[0].lock().unwrap().headers.clone();
    let (ticks, missed) = match (headers.first(), headers.last()) {
        (Some(first), Some(last)) => {
            let missed = last.missed.saturating_sub(first.missed) as f64;
            (((last.tick - first.tick) as f64 + 1.0 - missed).max(1.0), missed)
        }
        _ => (settings.seconds * 30.0, 0.0),
    };
    let mean_received = received.iter().sum::<f64>() / received.len().max(1) as f64;
    let result = json!({
        "clients": settings.clients,
        "connected": connected,
        "failedConnects": failures.len(),
        "seconds": settings.seconds,
        "ticks": ticks,
        "missedTicks": missed,
        "tickLatenessMs": spread(headers.iter().map(|header| header.lateness_us as f64 / 1000.0).collect()),
        "tickDurationMs": spread(headers.iter().map(|header| header.previous_tick_us as f64 / 1000.0).collect()),
        "interArrivalJitterMs": spread(jitter),
        "droppedShare": (1.0 - mean_received / ticks).max(0.0),
        "wrongSnapshots": wrong_size,
        "wrongPlayerCount": wrong_count,
    });
    eprintln!("{result}");
    result
}
