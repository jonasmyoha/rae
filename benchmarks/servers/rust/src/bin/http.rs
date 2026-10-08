// Phase 1 HTTP reference (benchmarks/servers/spec/Http.md): hyper 1.x on tokio.
// PORT (default 8080) and WORKERS (default 1: a current-thread runtime;
// more: a multi-thread runtime with that many worker threads).
use std::convert::Infallible;
use std::net::SocketAddr;

use bytes::Bytes;
use http_body_util::Full;
use hyper::header::{CONTENT_LENGTH, CONTENT_TYPE, SERVER};
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use serde::Serialize;
use tokio::net::TcpListener;

#[derive(Serialize)]
struct Message {
    message: &'static str,
}

fn response(status: StatusCode, content_type: &'static str, body: Bytes) -> Response<Full<Bytes>> {
    let length = body.len();
    let mut response = Response::new(Full::new(body));
    *response.status_mut() = status;
    let headers = response.headers_mut();
    headers.insert(SERVER, "hyper".parse().unwrap());
    headers.insert(CONTENT_TYPE, content_type.parse().unwrap());
    headers.insert(CONTENT_LENGTH, length.into());
    response
}

async fn handle(request: Request<hyper::body::Incoming>) -> Result<Response<Full<Bytes>>, Infallible> {
    Ok(match request.uri().path() {
        "/plaintext" => response(StatusCode::OK, "text/plain", Bytes::from_static(b"Hello, World!")),
        "/json" => {
            let body = serde_json::to_vec(&Message { message: "Hello, World!" }).unwrap();
            response(StatusCode::OK, "application/json", Bytes::from(body))
        }
        _ => response(StatusCode::NOT_FOUND, "text/plain", Bytes::from_static(b"Not Found")),
    })
}

async fn serve(port: u16) {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let listener = TcpListener::bind(address).await.expect("bind");
    println!("listening on {port}");
    loop {
        let (stream, _) = match listener.accept().await {
            Ok(accepted) => accepted,
            Err(_) => continue,
        };
        let _ = stream.set_nodelay(true);
        tokio::spawn(async move {
            let _ = http1::Builder::new()
                .pipeline_flush(true)
                .serve_connection(TokioIo::new(stream), service_fn(handle))
                .await;
        });
    }
}

fn main() {
    let port: u16 = std::env::var("PORT").ok().and_then(|text| text.parse().ok()).unwrap_or(8080);
    let workers: usize = std::env::var("WORKERS").ok().and_then(|text| text.parse().ok()).unwrap_or(1);
    let runtime = if workers <= 1 {
        tokio::runtime::Builder::new_current_thread().enable_all().build()
    } else {
        tokio::runtime::Builder::new_multi_thread().worker_threads(workers).enable_all().build()
    }
    .expect("runtime");
    runtime.block_on(serve(port));
}
