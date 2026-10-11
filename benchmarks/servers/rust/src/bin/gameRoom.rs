// Phase 3 game room reference (benchmarks/servers/spec/GameRoom.md), tuned:
// the shared WebSocket layer (src/ws.rs) on a current-thread tokio runtime
// (one worker, one room). PORT (default 8080). An input is applied to its
// player when it arrives. A clock thread sleeps to each 30 Hz deadline
// (thread::sleep, not tokio's millisecond timer wheel) and wakes the tick
// task, which encodes one snapshot and queues it on every player whose
// connection does not still hold more than one snapshot; the Hub writes them
// right after the tick.
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::net::SocketAddr;
use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant};

use tokio::net::{TcpListener, TcpStream};
use tokio::sync::Notify;

#[path = "../ws.rs"]
mod ws;

#[global_allocator]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;

const PERIOD: Duration = Duration::from_nanos(1_000_000_000 / 30);

struct Player {
    id: u32,
    x: i32,
    y: i32,
    last_sequence: u32,
    connection: Rc<ws::Connection>,
}

#[derive(Default)]
struct Room {
    // By connection, in join order
    players: BTreeMap<u64, Player>,
    next_player_id: u32,
}

type Shared = Rc<RefCell<Room>>;

fn apply_input(player: &mut Player, input: &[u8]) {
    let word = |at: usize| u32::from_le_bytes(input[at..at + 4].try_into().unwrap());
    let move_x = (word(4) as i32).clamp(-1000, 1000);
    let move_y = (word(8) as i32).clamp(-1000, 1000);
    player.last_sequence = word(0);
    player.x = (player.x + move_x).clamp(-1_000_000, 1_000_000);
    player.y = (player.y + move_y).clamp(-1_000_000, 1_000_000);
}

async fn connection(stream: TcpStream, key: u64, room: Shared, hub: Rc<ws::Hub>) {
    let Some((connection, mut reader)) = ws::accept(stream, &hub).await else {
        return;
    };
    {
        let mut room = room.borrow_mut();
        room.next_player_id += 1;
        let id = room.next_player_id;
        room.players.insert(key, Player { id, x: 0, y: 0, last_sequence: 0, connection: connection.clone() });
    }
    while let Some((opcode, input)) = reader.next(&connection).await {
        if opcode == ws::BINARY && input.len() == 32 {
            if let Some(player) = room.borrow_mut().players.get_mut(&key) {
                apply_input(player, &input);
            }
        }
    }
    room.borrow_mut().players.remove(&key);
    connection.finish().await;
}

// Sleeps to every deadline start + k × period and wakes the tick task; a tick
// still running at the next deadline gets one wake-up, not a backlog. macOS
// lets a sleep run long by a share of its length (timer coalescing: a 29 ms
// sleep wakes ~4 ms late), so a long wait sleeps half of what is left and
// repeats; the last sleeps are short and so is their slack (~0.3 ms).
fn clock(start: Instant, wake: Arc<Notify>) {
    let mut index = 1u32;
    loop {
        let deadline = start + PERIOD * index;
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                break;
            }
            if remaining > Duration::from_millis(1) {
                std::thread::sleep(remaining / 2);
            } else {
                std::thread::sleep(remaining);
            }
        }
        wake.notify_one();
        index += 1;
    }
}

async fn ticks(room: Shared) {
    let start = Instant::now();
    let wake = Arc::new(Notify::new());
    let clock_wake = wake.clone();
    std::thread::spawn(move || clock(start, clock_wake));
    let mut expected_index = 0u64;
    let mut missed = 0u64;
    let mut previous = Duration::ZERO;
    loop {
        let tick_start = Instant::now();
        let index = (tick_start.duration_since(start).as_nanos() / PERIOD.as_nanos()) as u64;
        missed += index.saturating_sub(expected_index);
        expected_index = index + 1;
        let lateness = tick_start.duration_since(start + PERIOD * index as u32);
        {
            let room = room.borrow();
            let mut snapshot = Vec::with_capacity(24 + 16 * room.players.len());
            for value in [
                index as u32,
                room.players.len() as u32,
                lateness.as_micros() as u32,
                previous.as_micros() as u32,
                missed as u32,
                0,
            ] {
                snapshot.extend_from_slice(&value.to_le_bytes());
            }
            for player in room.players.values() {
                snapshot.extend_from_slice(&player.id.to_le_bytes());
                snapshot.extend_from_slice(&player.x.to_le_bytes());
                snapshot.extend_from_slice(&player.y.to_le_bytes());
                snapshot.extend_from_slice(&player.last_sequence.to_le_bytes());
            }
            let frame = ws::frame(ws::BINARY, &snapshot);
            for player in room.players.values() {
                if player.connection.queued() <= snapshot.len() {
                    player.connection.send(&frame);
                }
            }
        }
        previous = tick_start.elapsed();
        wake.notified().await;
    }
}

async fn serve(port: u16) {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let listener = TcpListener::bind(address).await.expect("bind");
    println!("listening on {port}");
    let room: Shared = Rc::new(RefCell::new(Room::default()));
    let hub = ws::Hub::new();
    tokio::task::spawn_local(ticks(room.clone()));
    let mut next_key = 0u64;
    loop {
        let Ok((stream, _)) = listener.accept().await else {
            continue;
        };
        next_key += 1;
        tokio::task::spawn_local(connection(stream, next_key, room.clone(), hub.clone()));
    }
}

fn main() {
    let port: u16 = std::env::var("PORT").ok().and_then(|text| text.parse().ok()).unwrap_or(8080);
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("runtime");
    let local = tokio::task::LocalSet::new();
    local.block_on(&runtime, serve(port));
}
