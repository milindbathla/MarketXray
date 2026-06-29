// =============================================================================
// rust_ingestor/src/main.rs — NASDAQ ITCH 5.0 GZip Parser + SHM Producer
// =============================================================================

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufReader, Read};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use flate2::read::GzDecoder;

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

/// Must EXACTLY mirror mxray::Tick in Types.hpp
/// Layout: 8 + 8 + 4 + 4 + 1 + 1 + 38 padding = 64 bytes
#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct Tick {
    pub timestamp_ns: u64,
    pub order_id:     u64,
    pub price:        u32,   // Fixed-point: price * 100  ($150.00 → 15000)
    pub quantity:     u32,
    pub side:         Side,
    pub order_type:   OrderType,
    pub padding:      [u8; 38],
}

const _: () = assert!(std::mem::size_of::<Tick>()  == 64, "Tick must be 64 bytes!");
const _: () = assert!(std::mem::align_of::<Tick>() == 64, "Tick must be 64-byte aligned!");

// =============================================================================
// BUG FIX — Hawkes intensity time-domain mismatch
// =============================================================================
// ITCH timestamps are nanoseconds since midnight UTC on the trading day.
// The C++ HawkesProcess stores these, then calls compute_intensity(system_clock::now())
// which is nanoseconds since the Unix epoch (1970-01-01).
// The difference (~1.57 × 10^18 ns) causes exp(-β × Δt) ≈ 0, collapsing λ → μ = 0.5.
//
// Fix: shift every ITCH timestamp to Unix epoch nanoseconds by adding the
// number of nanoseconds from 1970-01-01 00:00:00 UTC to 2019-08-30 00:00:00 UTC.
const ITCH_DATE_OFFSET_NS: u64 = 1_567_123_200_000_000_000; // 2019-08-30 00:00:00 UTC

// =============================================================================
// Shared Memory layout — must match SharedMemory.hpp exactly
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
const _: () = assert!(std::mem::size_of::<SharedMemoryHeader>() == 64,
    "SharedMemoryHeader must be 64 bytes!");

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

// =============================================================================
// ShmProducer — POSIX shared memory producer (Linux)
// =============================================================================
pub struct ShmProducer {
    block: *mut SharedMemoryBlock,
    size:  usize,
}

unsafe impl Send for ShmProducer {}

impl ShmProducer {
    pub fn open(name: &str) -> Result<Self, String> {
        let size = std::mem::size_of::<SharedMemoryBlock>();
        use std::ffi::CString;
        let cname = CString::new(name).map_err(|e| e.to_string())?;

        let fd = loop {
            let fd = unsafe { libc::shm_open(cname.as_ptr(), libc::O_RDWR, 0o666) };
            if fd >= 0 { break fd; }
            std::thread::sleep(std::time::Duration::from_millis(50));
            eprint!(".");
        };
        eprintln!();

        let ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(), size,
                libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED, fd, 0,
            )
        };
        unsafe { libc::close(fd) };

        if ptr == libc::MAP_FAILED {
            return Err("mmap failed".into());
        }

        let block = ptr as *mut SharedMemoryBlock;
        let magic = unsafe { (*block).header.magic };
        if magic != SHM_MAGIC {
            return Err(format!(
                "SHM magic mismatch! Got 0x{:08X}, expected 0x{:08X}. \
                 Is the C++ daemon running?", magic, SHM_MAGIC));
        }
        Ok(ShmProducer { block, size })
    }

    pub fn signal_ready(&self) {
        unsafe {
            (*self.block).header.rust_ready.store(1, Ordering::Release);
        }
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
        unsafe { libc::munmap(self.block as *mut libc::c_void, self.size); }
    }
}

// =============================================================================
// ITCH 5.0 binary reading helpers (big-endian)
// =============================================================================

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

// =============================================================================
// Main
// =============================================================================

fn main() {
    println!("[Rust] NASDAQ ITCH 5.0 GZip Ingestor starting...");

    // -------------------------------------------------------------------------
    // 1. Open and decompress the ITCH .gz file
    // -------------------------------------------------------------------------
    let gz_paths = [
        "res/08302019.NASDAQ_ITCH50.gz",
        "../res/08302019.NASDAQ_ITCH50.gz",
        "/home/ash/acads/MarketXray/res/08302019.NASDAQ_ITCH50.gz",
    ];

    let raw_file = gz_paths.iter().find_map(|p| {
        File::open(p).ok().map(|f| { println!("[Rust] Found dataset: {}", p); f })
    }).unwrap_or_else(|| {
        eprintln!("[Rust] ERROR: NASDAQ ITCH .gz file not found!");
        std::process::exit(1);
    });

    let gz         = GzDecoder::new(raw_file);
    let mut reader = BufReader::with_capacity(1 << 20, gz);

    // -------------------------------------------------------------------------
    // 2. Open shared memory
    // -------------------------------------------------------------------------
    println!("[Rust] Waiting for C++ daemon to create shared memory region...");
    let producer = ShmProducer::open("/marketxray_shm").unwrap_or_else(|e| {
        eprintln!("[Rust] SHM open failed: {}", e);
        std::process::exit(1);
    });
    producer.signal_ready();
    println!("[Rust] Signaled ready. Streaming ticks into shared memory...");

    // -------------------------------------------------------------------------
    // 3. ITCH 5.0 parse loop
    // -------------------------------------------------------------------------

    // BUG FIX — VWAP and best_ask corruption:
    // ITCH 'E' (Execute) messages do NOT carry the order's price — only qty and
    // order_id. Previously we set price=0, which:
    //   (a) caused update_vwap(0, qty) to drag VWAP toward zero
    //   (b) added zero-price executes that confused the LOB ask-side tracking
    //
    // Fix: store (Side, original_price) in the map so Execute messages can
    // supply the real price that was on the book when the order was added.
    let mut order_side_map: HashMap<u64, (Side, u32)> = HashMap::with_capacity(2_000_000);

    let mut len_bytes      = [0u8; 2];
    let mut msg_count:     u64 = 0;
    let mut ticks_pushed:  u64 = 0;
    let mut ticks_dropped: u64 = 0;
    let mut last_reported  = std::time::Instant::now();

    while reader.read_exact(&mut len_bytes).is_ok() {
        let length = ((len_bytes[0] as usize) << 8) | (len_bytes[1] as usize);
        if length == 0 { continue; }

        let mut record = vec![0u8; length];
        if reader.read_exact(&mut record).is_err() {
            println!("[Rust] End of stream or read error — stopping.");
            break;
        }

        msg_count += 1;
        let t    = record[0];
        let body = &record[1..];

        let mut parsed_tick:    Option<Tick> = None;
        let mut secondary_tick: Option<Tick> = None;

        match t {
            b'A' | b'F' => {
                // Add Order (with / without MPID attribution)
                let order_id  = read_u64(body, 10);
                let side_byte = body[18];
                let side      = if side_byte == b'B' { Side::Buy } else { Side::Sell };
                let qty       = read_u32(body, 19);
                let price_raw = read_u32(body, 31);
                let price     = price_raw / 100;
                // BUG FIX: convert ITCH midnight-relative ns → Unix epoch ns
                let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;

                // Store (side, price) so Execute messages can look up the price
                order_side_map.insert(order_id, (side, price));

                parsed_tick = Some(Tick {
                    timestamp_ns: timestamp,
                    order_id, price, quantity: qty, side,
                    order_type: OrderType::Add,
                    padding: [0u8; 38],
                });
            }

            b'E' => {
                // Order Executed (no price in message — must look up from map)
                let order_id  = read_u64(body, 10);
                let qty       = read_u32(body, 18);
                let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                // BUG FIX: use the stored original order price, not 0
                let (side, orig_price) = order_side_map
                    .get(&order_id).copied().unwrap_or((Side::Buy, 0));

                parsed_tick = Some(Tick {
                    timestamp_ns: timestamp,
                    order_id,
                    price: orig_price, // was: 0 → VWAP corruption fixed
                    quantity: qty, side,
                    order_type: OrderType::Execute,
                    padding: [0u8; 38],
                });
            }

            b'C' => {
                // Order Executed with Price (price is in the message)
                let order_id  = read_u64(body, 10);
                let qty       = read_u32(body, 18);
                let price_raw = read_u32(body, 31);
                let price     = price_raw / 100;
                let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                let (side, _) = order_side_map
                    .get(&order_id).copied().unwrap_or((Side::Buy, 0));

                parsed_tick = Some(Tick {
                    timestamp_ns: timestamp,
                    order_id, price, quantity: qty, side,
                    order_type: OrderType::Execute,
                    padding: [0u8; 38],
                });
            }

            b'X' => {
                // Order Cancel (partial)
                let order_id  = read_u64(body, 10);
                let qty       = read_u32(body, 18);
                let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                let (side, orig_price) = order_side_map
                    .get(&order_id).copied().unwrap_or((Side::Buy, 0));

                parsed_tick = Some(Tick {
                    timestamp_ns: timestamp,
                    order_id,
                    price: orig_price,
                    quantity: qty, side,
                    order_type: OrderType::Cancel,
                    padding: [0u8; 38],
                });
            }

            b'D' => {
                // Order Delete (full cancel)
                let order_id  = read_u64(body, 10);
                let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                let (side, orig_price) = order_side_map
                    .remove(&order_id).unwrap_or((Side::Buy, 0));

                parsed_tick = Some(Tick {
                    timestamp_ns: timestamp,
                    order_id,
                    price: orig_price,
                    quantity: 0, side,
                    order_type: OrderType::Cancel,
                    padding: [0u8; 38],
                });
            }

            b'U' => {
                // Order Replace — cancel old + add new
                let orig_order_id = read_u64(body, 10);
                let new_order_id  = read_u64(body, 18);
                let qty           = read_u32(body, 26);
                let price_raw     = read_u32(body, 30);
                let price         = price_raw / 100;
                let timestamp     = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;
                let (side, old_price) = order_side_map
                    .remove(&orig_order_id).unwrap_or((Side::Buy, 0));

                order_side_map.insert(new_order_id, (side, price));

                // Cancel leg for the old order
                secondary_tick = Some(Tick {
                    timestamp_ns: timestamp,
                    order_id: orig_order_id,
                    price: old_price,
                    quantity: 0, side,
                    order_type: OrderType::Cancel,
                    padding: [0u8; 38],
                });

                parsed_tick = Some(Tick {
                    timestamp_ns: timestamp,
                    order_id: new_order_id,
                    price, quantity: qty, side,
                    order_type: OrderType::Add,
                    padding: [0u8; 38],
                });
            }

            b'P' => {
                // Non-Cross Trade (no order in the book — no map lookup needed)
                let match_id  = read_u64(body, 35);
                let side_byte = body[18];
                let side      = if side_byte == b'B' { Side::Buy } else { Side::Sell };
                let qty       = read_u32(body, 19);
                let price_raw = read_u32(body, 31);
                let price     = price_raw / 100;
                let timestamp = read_u48(body, 4) + ITCH_DATE_OFFSET_NS;

                parsed_tick = Some(Tick {
                    timestamp_ns: timestamp,
                    order_id: match_id,
                    price, quantity: qty, side,
                    order_type: OrderType::Execute,
                    padding: [0u8; 38],
                });
            }

            _ => {}
        }

        // Push cancel leg of Replace first, then the new Add
        if let Some(tick) = secondary_tick {
            push_with_backpressure(&producer, &tick, &mut ticks_pushed, &mut ticks_dropped);
        }
        if let Some(tick) = parsed_tick {
            push_with_backpressure(&producer, &tick, &mut ticks_pushed, &mut ticks_dropped);
        }

        if msg_count % 10_000_000 == 0 {
            let elapsed = last_reported.elapsed().as_secs_f64();
            if elapsed > 0.0 {
                println!(
                    "[Rust] {}M messages | {:.2}M msg/s | {} ticks pushed | {} dropped | book map: {}",
                    msg_count / 1_000_000,
                    10_000_000.0 / elapsed / 1e6,
                    ticks_pushed, ticks_dropped,
                    order_side_map.len()
                );
            }
            last_reported = std::time::Instant::now();
        }
    }

    println!(
        "[Rust] Done. Messages: {} | Pushed: {} | Dropped: {}",
        msg_count, ticks_pushed, ticks_dropped
    );
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
    const MAX_SPINS: u32 = 1000;
    let mut spins = 0u32;
    loop {
        if producer.push_tick(tick) { *pushed += 1; return; }
        spins += 1;
        if spins >= MAX_SPINS { *dropped += 1; return; }
        std::hint::spin_loop();
    }
}
