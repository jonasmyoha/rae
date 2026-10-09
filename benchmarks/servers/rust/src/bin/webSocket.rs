// Phase 2 WebSocket reference (benchmarks/servers/spec/WebSocket.md):
// tokio-tungstenite on a current-thread tokio runtime (one worker, as the spec
// asks). PORT (default 8080). Every connection has a writer task fed by a
// channel; a broadcast puts the same message (its bytes shared, not copied)
// on every connection's channel.
use std::cell::RefCell;
use std::collections::HashMap;
use std::net::SocketAddr;
use std::rc::Rc;

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc::{unbounded_channel, UnboundedSender};
use tokio_tungstenite::tungstenite::Message;

type Clients = Rc<RefCell<HashMap<u64, UnboundedSender<Message>>>>;

async fn connection(stream: TcpStream, id: u64, clients: Clients) {
    let _ = stream.set_nodelay(true);
    let Ok(socket) = tokio_tungstenite::accept_async(stream).await else {
        return;
    };
    let (mut writer, mut reader) = socket.split();
    let (sender, mut outbox) = unbounded_channel::<Message>();
    clients.borrow_mut().insert(id, sender.clone());
    let writing = tokio::task::spawn_local(async move {
        while let Some(message) = outbox.recv().await {
            if writer.send(message).await.is_err() {
                break;
            }
        }
    });
    while let Some(Ok(message)) = reader.next().await {
        let Message::Text(text) = message else {
            continue;
        };
        let Ok(value) = serde_json::from_str::<Value>(text.as_str()) else {
            continue;
        };
        match value.get("type").and_then(Value::as_str) {
            Some("echo") => {
                let _ = sender.send(Message::Text(text));
            }
            Some("broadcast") => {
                let payload = value.get("payload").cloned().unwrap_or(Value::Null);
                let broadcast = Message::text(json!({"type": "broadcast", "payload": payload}).to_string());
                let listen_count = {
                    let clients = clients.borrow();
                    for client in clients.values() {
                        let _ = client.send(broadcast.clone());
                    }
                    clients.len()
                };
                let result = json!({"type": "broadcastResult", "payload": payload, "listenCount": listen_count});
                let _ = sender.send(Message::text(result.to_string()));
            }
            _ => {}
        }
    }
    clients.borrow_mut().remove(&id);
    drop(sender);
    let _ = writing.await;
}

async fn serve(port: u16) {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let listener = TcpListener::bind(address).await.expect("bind");
    println!("listening on {port}");
    let clients: Clients = Rc::new(RefCell::new(HashMap::new()));
    let mut next_id = 0u64;
    loop {
        let Ok((stream, _)) = listener.accept().await else {
            continue;
        };
        next_id += 1;
        tokio::task::spawn_local(connection(stream, next_id, clients.clone()));
    }
}

fn main() {
    let port: u16 = std::env::var("PORT").ok().and_then(|text| text.parse().ok()).unwrap_or(8080);
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("runtime");
    let local = tokio::task::LocalSet::new();
    local.block_on(&runtime, serve(port));
}
