// Phase 3 game room reference (benchmarks/servers/spec/GameRoom.md):
// tokio-tungstenite on a current-thread tokio runtime (one worker, one room).
// PORT (default 8080). An input is applied to its player when it arrives; a
// tokio interval (missed ticks skipped) drives the 30 Hz tick, which encodes
// one snapshot and hands the same bytes to every player's writer task, except
// a player whose writer still holds more than one snapshot.
use std::cell::{Cell, RefCell};
use std::collections::BTreeMap;
use std::net::SocketAddr;
use std::rc::Rc;
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc::{unbounded_channel, UnboundedSender};
use tokio::time::{Instant, MissedTickBehavior};
use tokio_tungstenite::tungstenite::{Bytes, Message};

const PERIOD: Duration = Duration::from_nanos(1_000_000_000 / 30);

struct Player {
    id: u32,
    x: i32,
    y: i32,
    last_sequence: u32,
    sender: UnboundedSender<Message>,
    // Snapshot bytes handed to the writer and not yet written
    queued: Rc<Cell<usize>>,
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

async fn connection(stream: TcpStream, key: u64, room: Shared) {
    let _ = stream.set_nodelay(true);
    let Ok(socket) = tokio_tungstenite::accept_async(stream).await else {
        return;
    };
    let (mut writer, mut reader) = socket.split();
    let (sender, mut outbox) = unbounded_channel::<Message>();
    let queued = Rc::new(Cell::new(0usize));
    {
        let mut room = room.borrow_mut();
        room.next_player_id += 1;
        let id = room.next_player_id;
        room.players.insert(key, Player { id, x: 0, y: 0, last_sequence: 0, sender, queued: queued.clone() });
    }
    let writing = tokio::task::spawn_local(async move {
        while let Some(message) = outbox.recv().await {
            let length = message.len();
            let failed = writer.send(message).await.is_err();
            queued.set(queued.get().saturating_sub(length));
            if failed {
                break;
            }
        }
    });
    while let Some(Ok(message)) = reader.next().await {
        if let Message::Binary(input) = message {
            if input.len() == 32 {
                if let Some(player) = room.borrow_mut().players.get_mut(&key) {
                    apply_input(player, &input);
                }
            }
        }
    }
    room.borrow_mut().players.remove(&key);
    let _ = writing.await;
}

async fn ticks(room: Shared) {
    let start = Instant::now();
    let mut interval = tokio::time::interval_at(start, PERIOD);
    interval.set_missed_tick_behavior(MissedTickBehavior::Skip);
    let mut expected_index = 0u64;
    let mut missed = 0u64;
    let mut previous = Duration::ZERO;
    loop {
        let scheduled = interval.tick().await;
        let tick_start = Instant::now();
        let index = ((scheduled - start).as_nanos() / PERIOD.as_nanos()) as u64;
        missed += index.saturating_sub(expected_index);
        expected_index = index + 1;
        let lateness = tick_start.saturating_duration_since(scheduled);
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
        let length = snapshot.len();
        let message = Message::Binary(Bytes::from(snapshot));
        for player in room.players.values() {
            if player.queued.get() <= length {
                player.queued.set(player.queued.get() + length);
                let _ = player.sender.send(message.clone());
            }
        }
        previous = tick_start.elapsed();
    }
}

async fn serve(port: u16) {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let listener = TcpListener::bind(address).await.expect("bind");
    println!("listening on {port}");
    let room: Shared = Rc::new(RefCell::new(Room::default()));
    tokio::task::spawn_local(ticks(room.clone()));
    let mut next_key = 0u64;
    loop {
        let Ok((stream, _)) = listener.accept().await else {
            continue;
        };
        next_key += 1;
        tokio::task::spawn_local(connection(stream, next_key, room.clone()));
    }
}

fn main() {
    let port: u16 = std::env::var("PORT").ok().and_then(|text| text.parse().ok()).unwrap_or(8080);
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("runtime");
    let local = tokio::task::LocalSet::new();
    local.block_on(&runtime, serve(port));
}
