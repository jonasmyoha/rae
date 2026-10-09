// The WebSocket load client (spec/WebSocket.md "Load", decision F4), and
// with `gameRoom` first the game room's (spec/GameRoom.md, src/room.rs).
//
//   loadClient --url ws://127.0.0.1:8080/ws [--step 1000] [--max-clients 15000]
//              [--limit-ms 250] [--broadcasts 100] [--in-flight 4]
//              [--threads 4] [--connect-concurrency 64]
//
// Clients are connected `step` at a time. Every client reads every message
// it is sent. After each step `in-flight` clients send broadcasts, one at a
// time each, until `broadcasts` have been sent; a broadcast's round trip is
// from sending it to its sender receiving the broadcastResult, so the whole
// fan-out happens in between. The ramp stops when the step's p99 passes
// `limit-ms`, when a client cannot connect, or at `max-clients`. The result
// is one JSON object on stdout; progress goes to stderr.
mod room;

use std::time::{Duration, Instant};

use futures_util::stream::{SplitSink, StreamExt};
use futures_util::SinkExt;
use serde_json::{json, Value};
use tokio::net::TcpStream;
use tokio::sync::mpsc::{unbounded_channel, UnboundedReceiver, UnboundedSender};
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::WebSocketStream;

type Sink = SplitSink<WebSocketStream<TcpStream>, Message>;

struct Settings {
    url: String,
    step: usize,
    max_clients: usize,
    limit_ms: f64,
    broadcasts: usize,
    in_flight: usize,
    threads: usize,
    connect_concurrency: usize,
}

fn settings() -> Settings {
    let mut settings = Settings {
        url: "ws://127.0.0.1:8080/ws".to_string(),
        step: 1000,
        max_clients: 15000,
        limit_ms: 250.0,
        broadcasts: 100,
        in_flight: 4,
        threads: 4,
        connect_concurrency: 64,
    };
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i + 1 < arguments.len() {
        let value = arguments[i + 1].clone();
        let number = || value.parse::<usize>().expect("a number");
        match arguments[i].as_str() {
            "--url" => settings.url = value.clone(),
            "--step" => settings.step = number(),
            "--max-clients" => settings.max_clients = number(),
            "--limit-ms" => settings.limit_ms = value.parse().expect("a number"),
            "--broadcasts" => settings.broadcasts = number(),
            "--in-flight" => settings.in_flight = number(),
            "--threads" => settings.threads = number(),
            "--connect-concurrency" => settings.connect_concurrency = number(),
            other => panic!("unknown option {other}"),
        }
        i += 2;
    }
    settings
}

// A ramp holds thousands of sockets: raise the descriptor limit to the hard one
fn raise_file_limit() {
    unsafe {
        let mut limit = libc::rlimit { rlim_cur: 0, rlim_max: 0 };
        if libc::getrlimit(libc::RLIMIT_NOFILE, &mut limit) == 0 {
            limit.rlim_cur = limit.rlim_max.min(200_000);
            if libc::setrlimit(libc::RLIMIT_NOFILE, &limit) != 0 {
                limit.rlim_cur = 65536;
                libc::setrlimit(libc::RLIMIT_NOFILE, &limit);
            }
        }
    }
}

// What a sender's reader hands back: (round trip, listenCount)
type Results = UnboundedSender<(Duration, u64)>;

struct Client {
    sink: Option<Sink>,
    results: Option<UnboundedReceiver<(Duration, u64)>>,
}

async fn connect(url: String, address: String, start: Instant) -> Result<Client, String> {
    let stream = TcpStream::connect(&address).await.map_err(|error| error.to_string())?;
    let _ = stream.set_nodelay(true);
    // Reset on close instead of TIME_WAIT: back-to-back ramps would run out
    // of ephemeral ports (deprecated because a non-zero linger blocks on
    // drop; zero does not)
    #[allow(deprecated)]
    let _ = stream.set_linger(Some(Duration::ZERO));
    let (socket, _) = tokio_tungstenite::client_async(url, stream).await.map_err(|error| error.to_string())?;
    let (sink, mut stream) = socket.split();
    let (sender, receiver): (Results, _) = unbounded_channel();
    tokio::spawn(async move {
        while let Some(Ok(message)) = stream.next().await {
            let Message::Text(text) = message else {
                continue;
            };
            if !text.as_str().contains("broadcastResult") {
                continue;
            }
            let Ok(value) = serde_json::from_str::<Value>(text.as_str()) else {
                continue;
            };
            let sent = value["payload"]["sendTimeNs"].as_u64().unwrap_or(0);
            let now = start.elapsed().as_nanos() as u64;
            let listen_count = value["listenCount"].as_u64().unwrap_or(0);
            let _ = sender.send((Duration::from_nanos(now.saturating_sub(sent)), listen_count));
        }
    });
    Ok(Client { sink: Some(sink), results: Some(receiver) })
}

// One sender's share of a step's broadcasts, one at a time
async fn send_broadcasts(
    mut sink: Sink,
    mut results: UnboundedReceiver<(Duration, u64)>,
    count: usize,
    start: Instant,
) -> (Sink, UnboundedReceiver<(Duration, u64)>, Vec<(Duration, u64)>) {
    let mut measured = Vec::with_capacity(count);
    for _ in 0..count {
        let message = json!({
            "type": "broadcast",
            "payload": {"sendTimeNs": start.elapsed().as_nanos() as u64, "padding": "0123456789abcdef"}
        });
        if sink.send(Message::text(message.to_string())).await.is_err() {
            break;
        }
        match tokio::time::timeout(Duration::from_secs(10), results.recv()).await {
            Ok(Some(result)) => measured.push(result),
            _ => {
                measured.push((Duration::from_secs(10), 0));
                break;
            }
        }
    }
    (sink, results, measured)
}

fn percentile(sorted: &[f64], fraction: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    let index = ((sorted.len() as f64 - 1.0) * fraction).round() as usize;
    sorted[index]
}

async fn ramp(settings: Settings) -> Value {
    let start = Instant::now();
    let address = settings
        .url
        .trim_start_matches("ws://")
        .split('/')
        .next()
        .expect("host:port")
        .to_string();
    let mut clients: Vec<Client> = Vec::new();
    let mut steps = Vec::new();
    let mut reached = 0usize;
    let stopped_by;
    loop {
        if clients.len() >= settings.max_clients {
            stopped_by = "maxClients";
            break;
        }
        let wanted = settings.step.min(settings.max_clients - clients.len());
        let url = settings.url.clone();
        let connected: Vec<Result<Client, String>> = futures_util::stream::iter(0..wanted)
            .map(|_| connect(url.clone(), address.clone(), start))
            .buffer_unordered(settings.connect_concurrency)
            .collect()
            .await;
        let mut failed = None;
        for result in connected {
            match result {
                Ok(client) => clients.push(client),
                Err(error) => failed = Some(error),
            }
        }
        if let Some(error) = failed {
            eprintln!("connect failed at {} clients: {error}", clients.len());
            stopped_by = "connect";
            break;
        }
        tokio::time::sleep(Duration::from_millis(200)).await;

        let senders = settings.in_flight.min(clients.len()).max(1);
        let mut tasks = Vec::new();
        for (index, client) in clients.iter_mut().take(senders).enumerate() {
            let share = settings.broadcasts / senders + usize::from(index < settings.broadcasts % senders);
            let results = client.results.take().expect("results");
            let sink = client.sink.take().expect("sink");
            tasks.push(tokio::spawn(send_broadcasts(sink, results, share, start)));
        }
        let mut round_trips = Vec::new();
        let mut listen_count_errors = 0;
        for (index, task) in tasks.into_iter().enumerate() {
            let (sink, results, measured) = task.await.expect("sender");
            clients[index].sink = Some(sink);
            clients[index].results = Some(results);
            for (round_trip, listen_count) in measured {
                if listen_count != clients.len() as u64 {
                    listen_count_errors += 1;
                }
                round_trips.push(round_trip.as_secs_f64() * 1000.0);
            }
        }
        round_trips.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let p50 = percentile(&round_trips, 0.5);
        let p99 = percentile(&round_trips, 0.99);
        let max = round_trips.last().copied().unwrap_or(0.0);
        eprintln!(
            "{:6} clients: broadcast RTT p50 {:8.2} ms  p99 {:8.2} ms  max {:8.2} ms  ({} broadcasts, {} wrong listenCount)",
            clients.len(), p50, p99, max, round_trips.len(), listen_count_errors
        );
        steps.push(json!({
            "clients": clients.len(),
            "p50Ms": p50,
            "p99Ms": p99,
            "maxMs": max,
            "broadcasts": round_trips.len(),
            "listenCountErrors": listen_count_errors,
        }));
        if p99 > settings.limit_ms || round_trips.len() < settings.broadcasts {
            stopped_by = if round_trips.len() < settings.broadcasts { "timeout" } else { "p99" };
            break;
        }
        reached = clients.len();
    }
    json!({
        "steps": steps,
        "reachedClients": reached,
        "stoppedBy": stopped_by,
        "settings": {
            "step": settings.step,
            "maxClients": settings.max_clients,
            "limitMs": settings.limit_ms,
            "broadcastsPerStep": settings.broadcasts,
            "inFlight": settings.in_flight,
            "threads": settings.threads,
        },
    })
}

fn main() {
    raise_file_limit();
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    if arguments.first().map(String::as_str) == Some("gameRoom") {
        let settings = room::room_settings(&arguments[1..]);
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(settings.threads)
            .enable_all()
            .build()
            .expect("runtime");
        let result = runtime.block_on(room::game_room(settings));
        println!("{result}");
        std::process::exit(0);
    }
    let settings = settings();
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(settings.threads)
        .enable_all()
        .build()
        .expect("runtime");
    let result = runtime.block_on(ramp(settings));
    println!("{result}");
    // Drop the remaining sockets with the process, not one by one
    std::process::exit(0);
}
