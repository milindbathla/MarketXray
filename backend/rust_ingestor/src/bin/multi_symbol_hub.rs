use std::collections::BTreeMap;
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::Arc;
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::process::{Child, Command};
use tokio::sync::{broadcast, mpsc, Mutex};
use tokio::time::sleep;
use tokio_tungstenite::{accept_async, connect_async, tungstenite::Message};

struct ChildBundle {
    ws_child: Child,
    rust_child: Child,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = std::env::args().skip(1).collect::<Vec<_>>();

    let symbols = get_arg_value(&args, &["--symbols"])
        .unwrap_or_else(|| "AAPL,MSFT,NVDA,TSLA,AMZN".to_string())
        .split(',')
        .map(|s| s.trim().to_ascii_uppercase())
        .filter(|s| !s.is_empty())
        .collect::<Vec<_>>();

    let out_port = get_arg_value(&args, &["--out-port"])
        .and_then(|v| v.parse::<u16>().ok())
        .unwrap_or(9001);

    let base_port = get_arg_value(&args, &["--base-port"])
        .and_then(|v| v.parse::<u16>().ok())
        .unwrap_or(9101);

    let replay_speed = get_arg_value(&args, &["--replay-speed", "--speed"])
        .and_then(|v| v.parse::<f64>().ok())
        .unwrap_or(200.0);

    let snapshot_every = get_arg_value(&args, &["--snapshot-every"])
        .and_then(|v| v.parse::<u32>().ok())
        .unwrap_or(5);

    let snapshot_ms = get_arg_value(&args, &["--snapshot-ms"])
        .and_then(|v| v.parse::<u32>().ok())
        .unwrap_or(10);

    let backend_dir = std::env::current_dir()?.parent().unwrap().to_path_buf();
    let ws_binary = backend_dir.join("build/market_xray_ws");
    let rust_binary = resolve_rust_ingestor_binary()?;

    let (updates_tx, mut updates_rx) = mpsc::unbounded_channel::<(String, Value)>();
    let (broadcast_tx, _) = broadcast::channel::<String>(64);
    let latest = Arc::new(Mutex::new(BTreeMap::<String, Value>::new()));
    let mut children = Vec::new();

    for (idx, symbol) in symbols.iter().enumerate() {
        let port = base_port + idx as u16;
        let shm = format!("/marketxray_{}", symbol.to_ascii_lowercase());
        cleanup_stale_shm(&shm);

        let mut ws_child = Command::new(&ws_binary);
        ws_child
            .current_dir(&backend_dir)
            .arg("--port").arg(port.to_string())
            .arg("--shm").arg(&shm)
            .arg("--snapshot-every").arg(snapshot_every.to_string())
            .arg("--snapshot-ms").arg(snapshot_ms.to_string())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut ws_child = ws_child.spawn()?;
        if let Some(stdout) = ws_child.stdout.take() {
            tokio::spawn(pipe_logs(format!("ws:{symbol}"), stdout));
        }
        if let Some(stderr) = ws_child.stderr.take() {
            tokio::spawn(pipe_logs(format!("ws:{symbol}:err"), stderr));
        }
        sleep(Duration::from_millis(150)).await;

        let mut rust_child = Command::new(&rust_binary);
        rust_child
            .current_dir(backend_dir.join("rust_ingestor"))
            .arg("--symbol").arg(symbol)
            .arg("--shm").arg(&shm)
            .arg("--replay-speed").arg(replay_speed.to_string())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut rust_child = rust_child.spawn()?;
        if let Some(stdout) = rust_child.stdout.take() {
            tokio::spawn(pipe_logs(format!("rust:{symbol}"), stdout));
        }
        if let Some(stderr) = rust_child.stderr.take() {
            tokio::spawn(pipe_logs(format!("rust:{symbol}:err"), stderr));
        }

        children.push(ChildBundle { ws_child, rust_child });

        let tx = updates_tx.clone();
        let symbol_name = symbol.clone();
        tokio::spawn(async move {
            loop {
                match connect_async(format!("ws://127.0.0.1:{port}")).await {
                    Ok((stream, _)) => {
                        let (_, mut read) = stream.split();
                        while let Some(msg) = read.next().await {
                            match msg {
                                Ok(Message::Text(text)) => {
                                    if let Ok(snapshot) = serde_json::from_str::<Value>(&text) {
                                        let _ = tx.send((symbol_name.clone(), snapshot));
                                    }
                                }
                                Ok(Message::Binary(_)) => {}
                                Ok(Message::Close(_)) => break,
                                Err(_) => break,
                                _ => {}
                            }
                        }
                    }
                    Err(_) => sleep(Duration::from_millis(250)).await,
                }
            }
        });
    }

    let listener = TcpListener::bind(("0.0.0.0", out_port)).await?;
    println!("[hub] Multi-symbol feed on ws://0.0.0.0:{out_port}");
    println!("[hub] Symbols: {}", symbols.join(", "));
    println!("[hub] Snapshot cadence: every {snapshot_every} ticks, >= {snapshot_ms} ms");

    let latest_for_task = Arc::clone(&latest);
    let broadcast_for_task = broadcast_tx.clone();
    tokio::spawn(async move {
        while let Some((symbol, snapshot)) = updates_rx.recv().await {
            let mut guard = latest_for_task.lock().await;
            guard.insert(symbol.clone(), snapshot.clone());
            let payload = json!({
                "kind": "market_batch",
                "symbols": [{
                    "symbol": symbol,
                    "snapshot": snapshot
                }]
            });
            let _ = broadcast_for_task.send(payload.to_string());
        }
    });

    let accept_task = tokio::spawn(async move {
        loop {
            let (stream, _) = listener.accept().await?;
            let rx = broadcast_tx.subscribe();
            let latest = Arc::clone(&latest);
            tokio::spawn(handle_client(stream, rx, latest));
        }
        #[allow(unreachable_code)]
        Ok::<(), Box<dyn std::error::Error + Send + Sync>>(())
    });

    tokio::signal::ctrl_c().await?;
    accept_task.abort();
    for bundle in &mut children {
        let _ = bundle.rust_child.start_kill();
        let _ = bundle.ws_child.start_kill();
    }
    for bundle in &mut children {
        let _ = bundle.rust_child.wait().await;
        let _ = bundle.ws_child.wait().await;
    }
    Ok(())
}

async fn pipe_logs(tag: String, stream: impl tokio::io::AsyncRead + Unpin) {
    let mut lines = BufReader::new(stream).lines();
    while let Ok(Some(line)) = lines.next_line().await {
        println!("[{tag}] {line}");
    }
}

fn get_arg_value(args: &[String], names: &[&str]) -> Option<String> {
    for (idx, arg) in args.iter().enumerate() {
        for name in names {
            if let Some(value) = arg.strip_prefix(&format!("{name}=")) {
                return Some(value.to_string());
            }
            if arg == name {
                if let Some(next) = args.get(idx + 1) {
                    return Some(next.clone());
                }
            }
        }
    }
    None
}

fn cleanup_stale_shm(name: &str) {
    #[cfg(unix)]
    {
        use std::ffi::CString;
        if let Ok(cname) = CString::new(name) {
            unsafe {
                libc::shm_unlink(cname.as_ptr());
            }
        }
    }
}

fn resolve_rust_ingestor_binary() -> Result<PathBuf, Box<dyn std::error::Error>> {
    let mut path = std::env::current_dir()?;
    path.push("target");
    path.push("release");
    path.push(if cfg!(windows) { "rust_ingestor.exe" } else { "rust_ingestor" });
    if path.exists() {
        Ok(path)
    } else {
        Err(format!(
            "missing rust_ingestor binary at {}. Build it first with `cargo build --release --bin rust_ingestor --bin multi_symbol_hub`.",
            path.display()
        ).into())
    }
}

async fn handle_client(
    stream: TcpStream,
    mut rx: broadcast::Receiver<String>,
    latest: Arc<Mutex<BTreeMap<String, Value>>>,
) {
    let Ok(ws_stream) = accept_async(stream).await else {
        return;
    };
    let (mut write, _) = ws_stream.split();
    let initial_payload = {
        let guard = latest.lock().await;
        json!({
            "kind": "market_batch",
            "symbols": guard.iter().map(|(symbol, snapshot)| json!({
                "symbol": symbol,
                "snapshot": snapshot
            })).collect::<Vec<_>>()
        }).to_string()
    };
    if write.send(Message::Text(initial_payload)).await.is_err() {
        return;
    }
    while let Ok(payload) = rx.recv().await {
        if write.send(Message::Text(payload)).await.is_err() {
            break;
        }
    }
}
