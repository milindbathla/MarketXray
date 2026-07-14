// =============================================================================
// rust_ingestor/src/main.rs — NASDAQ ITCH 5.0 GZip Parser + Tokio Orchestrator
// =============================================================================
//
// Architecture:
//   [ITCH .gz] → spawn_blocking parse thread
//                   │ routes ticks per-symbol via SHM SPSC queue
//                   ▼
//           [C++ daemon per symbol — stdout JSON]
//                   │ async stdout readers
//                   ▼
//           [Arc<RwLock<SnapshotMap>>]  ← updated per snapshot
//                   │ 50 ms broadcast interval (only when changed)
//                   ▼
//           [tokio-tungstenite WS server :9001]
//                   │ market_batch JSON
//                   ▼
//               [Frontend]
//
// Fixes applied:
//   1. Composite order key (stock_locate, order_id) — prevents cross-symbol collision
//   2. RwLock dropped before WebSocket send().await
//   3. Per-symbol pacing anchor — no inter-symbol clock bias
//   4. Change-only broadcasts — skip 50 ms ticks with no new data
//   5. Graceful child shutdown via oneshot channel
// =============================================================================

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{BufReader, Read};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use flate2::read::GzDecoder;
use futures_util::SinkExt;
use serde_json::Value;
use tokio::io::{AsyncBufReadExt, BufReader as AsyncBufReader};
use tokio::net::TcpListener;
use tokio::process::Command;
use tokio::sync::{oneshot, RwLock};
use tokio_tungstenite::accept_async;

// =============================================================================
// Tick + enums — must EXACTLY mirror mxray::Tick / Side / OrderType in Types.hpp
// =============================================================================

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Side {
    Buy  = 1,
    Sell = 2,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum OrderType {
    Add     = 1,
    Cancel  = 2,
    Execute = 3,
}

#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct Tick {
    pub timestamp_ns: u64,
    pub order_id:     u64,
    pub price:        u32,
    pub quantity:     u32,
    pub side:         Side,
    pub order_type:   OrderType,
    pub trade_side_is_aggressor: u8,
    pub padding:      [u8; 37],
}

const _: () = assert!(std::mem::size_of::<Tick>()  == 64, "Tick must be 64 bytes!");
const _: () = assert!(std::mem::align_of::<Tick>() == 64, "Tick must be 64-byte aligned!");

const ITCH_DATE_OFFSET_NS: u64 = 1_567_123_200_000_000_000;
const MARKET_OPEN_NS: u64 = 34_200_000_000_000; // 09:30:00 ET

// =============================================================================
// Shared Memory layout — must mirror SharedMemory.hpp exactly
// =============================================================================

pub const SHM_MAGIC:  u32   = 0x4D585259;
pub const QUEUE_CAP:  usize = 65536;

#[repr(C, align(64))]
pub struct SharedMemoryHeader {
    pub magic:          u32,
    pub version:        u32,
    pub rust_ready:     AtomicU32,
    pub cpp_ready:      AtomicU32,
    pub queue_capacity: u64,
    pub padding:        [u8; 36],
}

#[repr(C, align(64))]
pub struct ShmSpscQueue {
    pub head:   AtomicU64,
    _pad_head:  [u8; 56],
    pub tail:   AtomicU64,
    _pad_tail:  [u8; 56],
    pub buffer: [Tick; QUEUE_CAP],
}

#[repr(C)]
pub struct SharedMemoryBlock {
    pub header: SharedMemoryHeader,
    pub queue:  ShmSpscQueue,
}

pub struct ShmProducer {
    block: *mut SharedMemoryBlock,
    size:  usize,
}

unsafe impl Send for ShmProducer {}

impl ShmProducer {
    #[cfg(unix)]
    pub fn open(name: &str) -> Result<Self, String> {
        let size = std::mem::size_of::<SharedMemoryBlock>();
        use std::ffi::CString;
        let cname = CString::new(name).map_err(|e| e.to_string())?;

        let fd = unsafe { libc::shm_open(cname.as_ptr(), libc::O_RDWR, 0o666) };
        if fd < 0 { return Err("shm_open failed".into()); }

        let ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(), size,
                libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED, fd, 0,
            )
        };
        unsafe { libc::close(fd) };

        if ptr == libc::MAP_FAILED { return Err("mmap failed".into()); }

        let block = ptr as *mut SharedMemoryBlock;
        let magic = unsafe { (*block).header.magic };
        if magic != SHM_MAGIC { return Err("magic mismatch".into()); }
        Ok(ShmProducer { block, size })
    }

    pub fn signal_ready(&self) {
        unsafe { (*self.block).header.rust_ready.store(1, Ordering::Release); }
    }

    #[inline(always)]
    pub fn push_tick(&self, tick: &Tick) -> bool {
        unsafe {
            let queue = &(*self.block).queue;
            let tail  = queue.tail.load(Ordering::Relaxed) as usize;
            let head  = queue.head.load(Ordering::Acquire) as usize;
            let next  = (tail + 1) & (QUEUE_CAP - 1);
            if next == head { return false; }
            (queue.buffer.as_ptr().add(tail) as *mut Tick).write(*tick);
            queue.tail.store(next as u64, Ordering::Release);
            true
        }
    }
}

impl Drop for ShmProducer {
    fn drop(&mut self) {
        #[cfg(unix)]
        unsafe { libc::munmap(self.block as *mut libc::c_void, self.size); }
    }
}

// =============================================================================
// Snapshot map: symbol → (version, snapshot JSON value)
// version increments on every new snapshot so the WS task only broadcasts
// when something actually changed.
// =============================================================================
type SnapshotMap = HashMap<String, (u64, Value)>;

// =============================================================================
// Main Orchestrator
// =============================================================================

#[tokio::main]
async fn main() {
    println!("[Rust] NASDAQ ITCH 5.0 GZip Ingestor Orchestrator starting...");

    let mut symbols_str = String::from("AAPL");
    let mut replay_speed = 1.0f64;

    let mut args_iter = std::env::args().skip(1);
    while let Some(arg) = args_iter.next() {
        match arg.as_str() {
            "--symbols" | "-s" | "--symbol" => {
                if let Some(s) = args_iter.next() {
                    symbols_str = s.trim().to_ascii_uppercase();
                }
            }
            "--replay-speed" | "--speed" => {
                if let Some(speed) = args_iter.next() {
                    if let Ok(parsed) = speed.parse::<f64>() {
                        replay_speed = parsed;
                    }
                }
            }
            _ => {}
        }
    }

    let target_symbols: Vec<String> = symbols_str
        .split(',')
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .collect();

    println!("[Rust] Target symbols: {:?}", target_symbols);
    println!("[Rust] Replay speed:   {:.2}x", replay_speed);

    // Shared snapshot store: symbol → (version, Value)
    let snapshots: Arc<RwLock<SnapshotMap>> = Arc::new(RwLock::new(HashMap::new()));

    // Reserve the browser gateway before launching workers. A failed bind used
    // to panic inside a detached task, leaving C++ engines running with no way
    // for the frontend to receive their snapshots.
    let listener = match TcpListener::bind("0.0.0.0:9001").await {
        Ok(listener) => listener,
        Err(error) => {
            eprintln!("[WS] Cannot bind ws://0.0.0.0:9001: {error}");
            return;
        }
    };
    println!("[WS] Listening on ws://0.0.0.0:9001");

    // Channel to signal async main when the blocking ingestor is done,
    // so we can gracefully kill child processes.
    let (done_tx, done_rx) = oneshot::channel::<()>();

    let mut children: Vec<tokio::process::Child> = Vec::new();
    let mut producers: HashMap<String, ShmProducer> = HashMap::new();

    // Spawn one C++ daemon per symbol
    for symbol in &target_symbols {
        let shm_name = format!("/mxray_{}", symbol);

        // FIX: Delete any stale SHM file before spawning C++ so we don't
        // accidentally open an old SHM region and race with C++'s O_TRUNC.
        #[cfg(unix)]
        let _ = std::fs::remove_file(format!("/dev/shm{}", shm_name));

        // Use CMake's output, not the stale convenience binary in backend/.
        let mut child = Command::new("../build/market_xray")
            .arg("--daemon")
            .arg("--shm")
            .arg(&shm_name)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::inherit()) // C++ stderr → terminal
            .spawn()
            .unwrap_or_else(|e| panic!("[Rust] Failed to spawn C++ daemon for {}: {}", symbol, e));

        eprintln!("[Rust] Spawned C++ daemon for {} on SHM {}", symbol, shm_name);

        // Async task: read C++ stdout, parse JSON snapshots, update shared map
        let stdout = child.stdout.take().expect("piped stdout");
        let snapshots_clone = snapshots.clone();
        let sym_clone = symbol.clone();

        tokio::spawn(async move {
            let mut reader = AsyncBufReader::new(stdout).lines();
            while let Ok(Some(line)) = reader.next_line().await {
                if !line.starts_with('{') { continue; }
                if let Ok(val) = serde_json::from_str::<Value>(&line) {
                    let mut map = snapshots_clone.write().await;
                    let entry = map.entry(sym_clone.clone()).or_insert((0, Value::Null));
                    entry.0 += 1; // bump version
                    entry.1 = val;
                }
            }
            eprintln!("[Rust] C++ daemon for {} exited", sym_clone);
        });

        children.push(child);

        // Wait until C++ daemon has created the SHM region (up to 5 s)
        eprintln!("[Rust] Waiting for SHM {}...", shm_name);
        let mut opened = false;
        for _ in 0..100 {
            if let Ok(prod) = ShmProducer::open(&shm_name) {
                prod.signal_ready();
                producers.insert(symbol.clone(), prod);
                opened = true;
                break;
            }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        if !opened {
            panic!("[Rust] Failed to open SHM {} within 5 seconds", shm_name);
        }
    }

    // -------------------------------------------------------------------------
    // WebSocket server — broadcasts market_batch to all connected clients
    // -------------------------------------------------------------------------
    let snapshots_ws = snapshots.clone();
    tokio::spawn(async move {
        while let Ok((stream, addr)) = listener.accept().await {
            let snaps = snapshots_ws.clone();
            tokio::spawn(async move {
                eprintln!("[WS] TCP connection from {}", addr);
                let Ok(mut ws_stream) = accept_async(stream).await else { return; };
                eprintln!("[WS] Handshake complete — client ready");

                // Per-client version tracking: only send when something changed
                let mut last_versions: HashMap<String, u64> = HashMap::new();
                let mut interval = tokio::time::interval(Duration::from_millis(50));

                loop {
                    interval.tick().await;

                    // FIX 3 & 6: Build JSON while holding lock briefly, then release
                    // BEFORE any network I/O. Only send if at least one symbol changed.
                    let json_str = {
                        let map = snaps.read().await;
                        if map.is_empty() { continue; }

                        // Check if anything changed since last broadcast
                        let changed = map.iter().any(|(sym, (ver, _))| {
                            last_versions.get(sym).copied().unwrap_or(0) < *ver
                        });
                        if !changed { continue; }

                        // Build the batch JSON
                        let symbols_arr: Vec<Value> = map
                            .iter()
                            .map(|(sym, (_, snap))| {
                                Value::Object({
                                    let mut m = serde_json::Map::new();
                                    m.insert("symbol".into(), Value::String(sym.clone()));
                                    m.insert("snapshot".into(), snap.clone());
                                    m
                                })
                            })
                            .collect();

                        // Update version tracking while still holding the read lock
                        let new_versions: Vec<(String, u64)> = map
                            .iter()
                            .map(|(sym, (ver, _))| (sym.clone(), *ver))
                            .collect();

                        let mut batch = serde_json::Map::new();
                        batch.insert("kind".into(), Value::String("market_batch".into()));
                        batch.insert("symbols".into(), Value::Array(symbols_arr));
                        let json = serde_json::to_string(&Value::Object(batch)).unwrap();

                        (json, new_versions)
                        // ← RwLock dropped here, BEFORE send
                    };

                    // Destructure the tuple returned from the block
                    let (json, new_versions) = json_str;

                    // Update version tracking (no lock held)
                    for (sym, ver) in new_versions {
                        last_versions.insert(sym, ver);
                    }

                    // Network I/O with NO lock held
                    if ws_stream
                        .send(tokio_tungstenite::tungstenite::Message::Text(json.into()))
                        .await
                        .is_err()
                    {
                        eprintln!("[WS] Client {} disconnected", addr);
                        break;
                    }
                }
            });
        }
    });

    // -------------------------------------------------------------------------
    // ITCH parsing — runs in a dedicated blocking thread (sync I/O + CPU)
    // -------------------------------------------------------------------------
    let target_symbols_set: HashSet<String> = target_symbols.into_iter().collect();

    tokio::task::spawn_blocking(move || {
        run_ingestor(target_symbols_set, producers, replay_speed);
        // Signal async main that the file is done
        let _ = done_tx.send(());
    });

    // Wait for ITCH file to finish, then gracefully kill children
    let _ = done_rx.await;
    eprintln!("[Rust] ITCH file done — shutting down C++ daemons...");

    for mut child in children {
        let _ = child.kill().await;
        let _ = child.wait().await;
    }

    eprintln!("[Rust] All daemons stopped. Exiting.");
}

// =============================================================================
// ITCH 5.0 binary parser — runs synchronously in spawn_blocking
// =============================================================================

fn run_ingestor(
    target_symbols: HashSet<String>,
    producers: HashMap<String, ShmProducer>,
    replay_speed: f64,
) {
    let gz_paths = [
        "res/08302019.NASDAQ_ITCH50.gz",
        "../res/08302019.NASDAQ_ITCH50.gz",
        "../08302019.NASDAQ_ITCH50.gz",
        "../../08302019.NASDAQ_ITCH50.gz",
    ];

    let raw_file = gz_paths
        .iter()
        .find_map(|p| {
            File::open(p).ok().map(|f| {
                eprintln!("[Rust] Found dataset: {}", p);
                f
            })
        })
        .unwrap_or_else(|| {
            eprintln!("[Rust] ERROR: NASDAQ ITCH .gz file not found!");
            std::process::exit(1);
        });

    let gz = GzDecoder::new(raw_file);
    let mut reader = BufReader::with_capacity(1 << 20, gz);

    // FIX 1: Composite key (stock_locate, order_id) prevents cross-symbol
    // order ID collisions. NASDAQ only guarantees order ID uniqueness per stock.
    let mut order_side_map: HashMap<(u16, u64), (Side, u32)> =
        HashMap::with_capacity(2_000_000);

    // Maps stock_locate code → symbol string (populated from 'R' messages)
    let mut target_stock_locates: HashMap<u16, String> = HashMap::new();

    // FIX 5: Per-symbol pacing anchor — prevents AAPL's early timestamp from
    // biasing TSLA's first tick and causing a large spurious sleep.
    let mut replay_anchors: HashMap<String, (Option<u64>, Instant)> = producers
        .keys()
        .map(|sym| (sym.clone(), (None, Instant::now())))
        .collect();

    // Scratch buffer reused for every ITCH message (avoids per-message allocation)
    let mut record: Vec<u8> = Vec::with_capacity(128);
    let mut len_bytes = [0u8; 2];

    let mut msg_count: u64 = 0;
    let mut ticks_pushed: u64 = 0;
    let mut ticks_dropped: u64 = 0;
    let mut last_reported = Instant::now();

    while reader.read_exact(&mut len_bytes).is_ok() {
        let length = ((len_bytes[0] as usize) << 8) | (len_bytes[1] as usize);
        if length == 0 { continue; }

        // Reuse scratch buffer — no heap allocation on the hot path
        record.resize(length, 0);
        if reader.read_exact(&mut record).is_err() { break; }

        msg_count += 1;
        let t    = record[0];
        let body = &record[1..];

        let mut parsed_tick:    Option<Tick>    = None;
        let mut secondary_tick: Option<Tick>    = None;
        let mut tick_symbol:    Option<&String> = None;

        match t {
            // Stock Directory — map stock_locate codes to symbol strings
            b'R' => {
                let stock_locate = read_u16(body, 0);
                let symbol = read_stock_symbol(body, 10);
                if !symbol.is_empty() && target_symbols.contains(&symbol) {
                    target_stock_locates.insert(stock_locate, symbol.clone());
                    eprintln!("[Rust] Resolved {} → stock_locate {}", symbol, stock_locate);
                }
            }

            // Add Order (with or without MPID attribution)
            b'A' | b'F' => {
                let stock_locate = read_u16(body, 0);
                if let Some(sym) = target_stock_locates.get(&stock_locate) {
                    tick_symbol = Some(sym);
                    let order_id  = read_u64(body, 10);
                    let side      = if body[18] == b'B' { Side::Buy } else { Side::Sell };
                    let qty       = read_u32(body, 19);
                    let price     = read_u32(body, 31) / 10000;
                    let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;

                    // FIX 1: composite key
                    order_side_map.insert((stock_locate, order_id), (side, price));

                    parsed_tick = Some(Tick {
                        timestamp_ns: timestamp, order_id, price,
                        quantity: qty, side, order_type: OrderType::Add, trade_side_is_aggressor: 0, padding: [0; 37],
                    });
                }
            }

            // Order Executed
            b'E' => {
                let stock_locate = read_u16(body, 0);
                if let Some(sym) = target_stock_locates.get(&stock_locate) {
                    tick_symbol = Some(sym);
                    let order_id  = read_u64(body, 10);
                    let qty       = read_u32(body, 18);
                    let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                    // FIX 1: composite key
                    if let Some(&(side, orig_price)) = order_side_map.get(&(stock_locate, order_id)) {
                        parsed_tick = Some(Tick {
                            timestamp_ns: timestamp, order_id,
                            price: orig_price, quantity: qty,
                            side, order_type: OrderType::Execute, trade_side_is_aggressor: 0, padding: [0; 37],
                        });
                    }
                }
            }

            // Order Executed With Price (different fill price)
            b'C' => {
                let stock_locate = read_u16(body, 0);
                if let Some(sym) = target_stock_locates.get(&stock_locate) {
                    tick_symbol = Some(sym);
                    let order_id  = read_u64(body, 10);
                    let qty       = read_u32(body, 18);
                    let price     = read_u32(body, 31) / 10000;
                    let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                    // FIX 1: composite key
                    if let Some(&(side, _)) = order_side_map.get(&(stock_locate, order_id)) {
                        parsed_tick = Some(Tick {
                            timestamp_ns: timestamp, order_id, price,
                            quantity: qty, side, order_type: OrderType::Execute, trade_side_is_aggressor: 0, padding: [0; 37],
                        });
                    }
                }
            }

            // Order Cancel (partial)
            b'X' => {
                let stock_locate = read_u16(body, 0);
                if let Some(sym) = target_stock_locates.get(&stock_locate) {
                    tick_symbol = Some(sym);
                    let order_id  = read_u64(body, 10);
                    let qty       = read_u32(body, 18);
                    let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                    // FIX 1: composite key
                    if let Some(&(side, orig_price)) = order_side_map.get(&(stock_locate, order_id)) {
                        parsed_tick = Some(Tick {
                            timestamp_ns: timestamp, order_id,
                            price: orig_price, quantity: qty,
                            side, order_type: OrderType::Cancel, trade_side_is_aggressor: 0, padding: [0; 37],
                        });
                    }
                }
            }

            // Order Delete (full cancel)
            b'D' => {
                let stock_locate = read_u16(body, 0);
                if let Some(sym) = target_stock_locates.get(&stock_locate) {
                    tick_symbol = Some(sym);
                    let order_id  = read_u64(body, 10);
                    let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                    // FIX 1: composite key (remove consumes the entry)
                    if let Some((side, orig_price)) = order_side_map.remove(&(stock_locate, order_id)) {
                        parsed_tick = Some(Tick {
                            timestamp_ns: timestamp, order_id,
                            price: orig_price, quantity: 0,
                            side, order_type: OrderType::Cancel, trade_side_is_aggressor: 0, padding: [0; 37],
                        });
                    }
                }
            }

            // Order Replace — cancel old order + add new order at new price/qty
            b'U' => {
                let stock_locate = read_u16(body, 0);
                if let Some(sym) = target_stock_locates.get(&stock_locate) {
                    tick_symbol = Some(sym);
                    let orig_order_id = read_u64(body, 10);
                    let new_order_id  = read_u64(body, 18);
                    let qty           = read_u32(body, 26);
                    let price         = read_u32(body, 30) / 10000;
                    let timestamp     = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                    // FIX 1: composite key
                    if let Some((side, old_price)) = order_side_map.remove(&(stock_locate, orig_order_id)) {
                        order_side_map.insert((stock_locate, new_order_id), (side, price));

                        secondary_tick = Some(Tick {
                            timestamp_ns: timestamp,
                            order_id: orig_order_id, price: old_price,
                            quantity: 0, side, order_type: OrderType::Cancel, trade_side_is_aggressor: 0, padding: [0; 37],
                        });
                        parsed_tick = Some(Tick {
                            timestamp_ns: timestamp,
                            order_id: new_order_id, price,
                            quantity: qty, side, order_type: OrderType::Add, trade_side_is_aggressor: 0, padding: [0; 37],
                        });
                    }
                }
            }

            // Non-Cross Trade Message (no order-book entry)
            b'P' => {
                let stock_locate = read_u16(body, 0);
                if let Some(sym) = target_stock_locates.get(&stock_locate) {
                    tick_symbol = Some(sym);
                    let match_id  = read_u64(body, 35);
                    let side      = if body[18] == b'B' { Side::Buy } else { Side::Sell };
                    let qty       = read_u32(body, 19);
                    let price     = read_u32(body, 31) / 10000;
                    let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                    parsed_tick = Some(Tick {
                        timestamp_ns: timestamp,
                        order_id: match_id, price,
                        quantity: qty, side, order_type: OrderType::Execute, trade_side_is_aggressor: 1, padding: [0; 37],
                    });
                }
            }

            _ => {}
        }

        // Route ticks to the correct symbol's SHM producer
        if let Some(sym) = tick_symbol {
            if let Some(prod) = producers.get(sym) {
                // FIX 5: per-symbol pacing anchor
                let (ref mut anchor_ts, ref mut anchor_wall) = *replay_anchors.get_mut(sym).unwrap();

                if let Some(tick) = secondary_tick {
                    pace_replay(tick.timestamp_ns, replay_speed, anchor_ts, anchor_wall);
                    push_with_backpressure(prod, &tick, &mut ticks_pushed, &mut ticks_dropped);
                }
                if let Some(tick) = parsed_tick {
                    pace_replay(tick.timestamp_ns, replay_speed, anchor_ts, anchor_wall);
                    push_with_backpressure(prod, &tick, &mut ticks_pushed, &mut ticks_dropped);
                }
            }
        }

        if msg_count % 10_000_000 == 0 {
            let elapsed = last_reported.elapsed().as_secs_f64();
            if elapsed > 0.0 {
                eprintln!(
                    "[Rust] {}M messages | {:.2}M msg/s | {} ticks pushed | {} dropped | order map: {}",
                    msg_count / 1_000_000,
                    10_000_000.0 / elapsed / 1e6,
                    ticks_pushed,
                    ticks_dropped,
                    order_side_map.len(),
                );
            }
            last_reported = Instant::now();
        }
    }

    eprintln!(
        "[Rust] Done. Messages: {} | Pushed: {} | Dropped: {}",
        msg_count, ticks_pushed, ticks_dropped
    );

    // Give C++ daemons a moment to drain remaining ticks and emit final snapshots
    std::thread::sleep(Duration::from_secs(2));
    // done_tx is dropped here, which signals the async main to kill children
}

// =============================================================================
// Helpers
// =============================================================================

#[inline]
fn push_with_backpressure(
    producer: &ShmProducer,
    tick: &Tick,
    pushed: &mut u64,
    dropped: &mut u64,
) {
    const MAX_SPINS: u32 = 1_000;
    let mut spins = 0u32;
    loop {
        if producer.push_tick(tick) { *pushed += 1; return; }
        spins += 1;
        if spins >= MAX_SPINS { *dropped += 1; return; }
        std::hint::spin_loop();
    }
}

#[inline]
fn pace_replay(
    tick_ts_ns: u64,
    replay_speed: f64,
    replay_anchor_ts_ns: &mut Option<u64>,
    replay_anchor_wall: &mut Instant,
) {
    if tick_ts_ns < ITCH_DATE_OFFSET_NS + MARKET_OPEN_NS {
        return; // Do not pace before the regular-session open.
    }

    if replay_anchor_ts_ns.is_none() {
        *replay_anchor_ts_ns = Some(tick_ts_ns);
        *replay_anchor_wall = Instant::now(); // Reset wall clock at 09:30 ET.
    }

    let start_ts = replay_anchor_ts_ns.unwrap();
    let source_elapsed_ns = tick_ts_ns.saturating_sub(start_ts);
    let target_elapsed = Duration::from_secs_f64(source_elapsed_ns as f64 / 1e9 / replay_speed);
    let wall_elapsed = replay_anchor_wall.elapsed();

    if target_elapsed > wall_elapsed {
        std::thread::sleep(target_elapsed - wall_elapsed);
    }
}

#[inline(always)]
fn read_u16(buf: &[u8], offset: usize) -> u16 {
    ((buf[offset] as u16) << 8) | (buf[offset + 1] as u16)
}

#[inline(always)]
fn read_u32(buf: &[u8], offset: usize) -> u32 {
    ((buf[offset]     as u32) << 24)
        | ((buf[offset + 1] as u32) << 16)
        | ((buf[offset + 2] as u32) <<  8)
        |  (buf[offset + 3] as u32)
}

#[inline(always)]
fn read_u64(buf: &[u8], offset: usize) -> u64 {
    ((buf[offset]     as u64) << 56)
        | ((buf[offset + 1] as u64) << 48)
        | ((buf[offset + 2] as u64) << 40)
        | ((buf[offset + 3] as u64) << 32)
        | ((buf[offset + 4] as u64) << 24)
        | ((buf[offset + 5] as u64) << 16)
        | ((buf[offset + 6] as u64) <<  8)
        |  (buf[offset + 7] as u64)
}

#[inline(always)]
fn read_u48(buf: &[u8], offset: usize) -> u64 {
    ((buf[offset]     as u64) << 40)
        | ((buf[offset + 1] as u64) << 32)
        | ((buf[offset + 2] as u64) << 24)
        | ((buf[offset + 3] as u64) << 16)
        | ((buf[offset + 4] as u64) <<  8)
        |  (buf[offset + 5] as u64)
}

#[inline(always)]
fn read_stock_symbol(buf: &[u8], offset: usize) -> String {
    std::str::from_utf8(&buf[offset..offset + 8])
        .unwrap_or("")
        .trim()
        .to_ascii_uppercase()
}
