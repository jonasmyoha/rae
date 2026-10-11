// Phase 2 WebSocket reference (benchmarks/servers/spec/WebSocket.md), tuned:
// the shared WebSocket layer (src/ws.rs) on a current-thread tokio runtime
// (one worker, as the spec asks). PORT (default 8080). A broadcast encodes its
// frame once and queues it on every client in accept order (the Hub writes
// them after the batch); the payload is passed through as raw JSON, never
// re-serialised.
use std::borrow::Cow;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::net::SocketAddr;
use std::rc::Rc;

use serde::Deserialize;
use serde_json::value::RawValue;
use tokio::net::{TcpListener, TcpStream};

#[path = "../ws.rs"]
mod ws;

#[global_allocator]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;

type Clients = Rc<RefCell<BTreeMap<u64, Rc<ws::Connection>>>>;

#[derive(Deserialize)]
struct Incoming<'a> {
    #[serde(rename = "type", borrow)]
    kind: Cow<'a, str>,
    #[serde(borrow)]
    payload: Option<&'a RawValue>,
}

async fn connection(stream: TcpStream, id: u64, clients: Clients, hub: Rc<ws::Hub>) {
    let Some((connection, mut reader)) = ws::accept(stream, &hub).await else {
        return;
    };
    clients.borrow_mut().insert(id, connection.clone());
    while let Some((opcode, message)) = reader.next(&connection).await {
        if opcode != ws::TEXT {
            continue;
        }
        let Ok(incoming) = serde_json::from_slice::<Incoming>(&message) else {
            continue;
        };
        let payload = incoming.payload.map_or("null", RawValue::get);
        match incoming.kind.as_ref() {
            "echo" => connection.send(&ws::frame(ws::TEXT, &message)),
            "broadcast" => {
                let broadcast = ws::frame(ws::TEXT, format!(r#"{{"type":"broadcast","payload":{payload}}}"#).as_bytes());
                let listen_count = {
                    let clients = clients.borrow();
                    for client in clients.values() {
                        client.send(&broadcast);
                    }
                    clients.len()
                };
                let result = format!(r#"{{"type":"broadcastResult","payload":{payload},"listenCount":{listen_count}}}"#);
                connection.send(&ws::frame(ws::TEXT, result.as_bytes()));
            }
            _ => {}
        }
    }
    clients.borrow_mut().remove(&id);
    connection.finish().await;
}

async fn serve(port: u16) {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let listener = TcpListener::bind(address).await.expect("bind");
    println!("listening on {port}");
    let clients: Clients = Rc::new(RefCell::new(BTreeMap::new()));
    let hub = ws::Hub::new();
    let mut next_id = 0u64;
    loop {
        let Ok((stream, _)) = listener.accept().await else {
            continue;
        };
        next_id += 1;
        tokio::task::spawn_local(connection(stream, next_id, clients.clone(), hub.clone()));
    }
}

fn main() {
    let port: u16 = std::env::var("PORT").ok().and_then(|text| text.parse().ok()).unwrap_or(8080);
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("runtime");
    let local = tokio::task::LocalSet::new();
    local.block_on(&runtime, serve(port));
}
