# Rust Ingestor — Shared Memory Bridge Guide

This document explains exactly how the Rust side must write ticks into the Shared Memory region that the C++ engine reads from.

## How It Works

```
[NASDAQ ITCH / CSV] → Rust Parser (tokio) → SHMSPSCQueue (shared RAM) → C++ Analytics Engine
```

- C++ creates the named shared memory region and waits.
- Rust opens the SAME region, writes parsed `Tick` structs into the ring buffer, and sets `rust_ready = 1`.
- C++ wakes up, reads from the ring buffer at sub-100ns latency.
- **Zero network stack. Zero serialization. Zero copy.**

---

## Step 1 — Add Dependencies (`Cargo.toml`)

```toml
[dependencies]
tokio = { version = "1", features = ["full"] }

[target.'cfg(windows)'.dependencies]
windows = { version = "0.51", features = ["Win32_System_Memory", "Win32_Foundation"] }

[target.'cfg(unix)'.dependencies]
libc = "0.2"
```

---

## Step 2 — Mirror the C++ Tick Struct EXACTLY

The C++ `Tick` is `#pragma pack(1)` + `alignas(64)` = exactly **64 bytes**.
Rust must use `#[repr(C, align(64))]` and `#[repr(u8)]` enums to match.

```rust
/// Must EXACTLY mirror mxray::Side in Types.hpp
#[repr(u8)]
#[derive(Clone, Copy, Debug)]
pub enum Side {
    Buy  = 1,
    Sell = 2,
}

/// Must EXACTLY mirror mxray::OrderType in Types.hpp
#[repr(u8)]
#[derive(Clone, Copy, Debug)]
pub enum OrderType {
    Add     = 1,
    Cancel  = 2,
    Execute = 3,
}

/// Must EXACTLY mirror mxray::Tick in Types.hpp
/// Size = 8 + 8 + 4 + 4 + 1 + 1 + 38 padding = 64 bytes
#[repr(C, align(64))]
#[derive(Clone, Copy)]
pub struct Tick {
    pub timestamp_ns: u64,
    pub order_id:     u64,
    pub price:        u32,   // Fixed-point: price * 100 (e.g. $150.00 = 15000)
    pub quantity:     u32,
    pub side:         Side,
    pub order_type:   OrderType,
    pub padding:      [u8; 38],
}

impl Tick {
    pub fn new_add(order_id: u64, price: u32, qty: u32, side: Side) -> Self {
        Tick {
            timestamp_ns: now_ns(),
            order_id,
            price,
            quantity: qty,
            side,
            order_type: OrderType::Add,
            padding: [0u8; 38],
        }
    }
}

fn now_ns() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos() as u64
}
```

**VERIFY SIZE:** Add this assertion in your Rust code:
```rust
const _: () = assert!(std::mem::size_of::<Tick>() == 64, "Tick must be 64 bytes!");
const _: () = assert!(std::mem::align_of::<Tick>() == 64, "Tick must be 64-byte aligned!");
```

---

## Step 3 — Mirror the Shared Memory Header

```rust
/// Must match mxray::SharedMemoryHeader in SharedMemory.hpp
#[repr(C, align(64))]
pub struct SharedMemoryHeader {
    pub magic:      u32,
    pub version:    u32,
    pub rust_ready: std::sync::atomic::AtomicU32,
    pub cpp_ready:  std::sync::atomic::AtomicU32,
    pub queue_cap:  u64,
    pub padding:    [u8; 36],
}

pub const SHM_MAGIC:   u32 = 0x4D585259; // "MXRY"
pub const SHM_VERSION: u32 = 1;
pub const QUEUE_CAP:   usize = 65536;
```

---

## Step 4 — Open Shared Memory (Windows)

```rust
use std::sync::atomic::{AtomicU32, Ordering};
use windows::Win32::System::Memory::*;
use windows::Win32::Foundation::*;

pub struct ShmProducer {
    handle: HANDLE,
    ptr:    *mut u8,
    size:   usize,
}

impl ShmProducer {
    pub fn open(name: &str) -> Result<Self, String> {
        let size = std::mem::size_of::<SharedMemoryHeader>()
                 + std::mem::size_of::<[Tick; QUEUE_CAP]>()
                 + 128; // atomics padding

        let name_w: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();

        unsafe {
            // Wait for C++ to create the SHM region
            let handle = loop {
                let h = OpenFileMappingW(FILE_MAP_ALL_ACCESS.0, false, name_w.as_ptr());
                if let Ok(h) = h { break h; }
                std::thread::sleep(std::time::Duration::from_millis(10));
            };

            let ptr = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, size)
                .0 as *mut u8;

            if ptr.is_null() {
                return Err("MapViewOfFile failed".into());
            }

            Ok(ShmProducer { handle, ptr, size })
        }
    }

    /// Get a reference to the SPSC queue's raw atomic tail
    pub fn push_tick(&self, tick: &Tick) -> bool {
        // Offset: header is first 64 bytes, then two 64-byte atomics, then buffer
        // Layout must match SHMSPSCQueue<Tick, 65536> in SharedMemory.hpp
        unsafe {
            let header = &*(self.ptr as *const SharedMemoryHeader);

            // Validate magic before writing
            assert_eq!(header.magic, SHM_MAGIC, "SHM magic mismatch!");

            // head and tail atomics are at offset 64 (header) + 0 and 64 (each on own cache line)
            let queue_base = self.ptr.add(64); // after header
            let head_ptr   = queue_base as *const AtomicU32;
            let tail_ptr   = queue_base.add(64) as *const AtomicU32; // 64-byte aligned after head

            let head = (*head_ptr).load(Ordering::Acquire) as usize;
            let tail = (*tail_ptr).load(Ordering::Relaxed) as usize;

            let next_tail = (tail + 1) & (QUEUE_CAP - 1);
            if next_tail == head {
                return false; // Queue full
            }

            // Buffer starts at offset 64 (header) + 64 (head atomic) + 64 (tail atomic)
            let buf_base = queue_base.add(128) as *mut Tick;
            *buf_base.add(tail) = *tick;

            (*tail_ptr as *const AtomicU32 as *mut AtomicU32)
                .store(next_tail as u32, Ordering::Release);

            true
        }
    }

    pub fn signal_ready(&self) {
        unsafe {
            let header = &*(self.ptr as *const SharedMemoryHeader);
            header.rust_ready.store(1, Ordering::Release);
        }
    }
}
```

---

## Step 5 — POSIX (Linux) Shared Memory

```rust
// Linux: use libc::shm_open + mmap
// The C++ side sets O_CREAT — Rust uses O_RDWR only (no O_CREAT)
extern "C" {
    fn shm_open(name: *const i8, oflag: i32, mode: u32) -> i32;
}

pub fn open_posix_shm(name: &str, size: usize) -> *mut u8 {
    use std::ffi::CString;
    let cname = CString::new(name).unwrap();
    let fd = unsafe { shm_open(cname.as_ptr(), libc::O_RDWR, 0o666) };
    assert!(fd >= 0, "shm_open failed");

    let ptr = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            size,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            fd,
            0,
        )
    };
    assert_ne!(ptr, libc::MAP_FAILED, "mmap failed");
    unsafe { libc::close(fd) };
    ptr as *mut u8
}
```

---

## Step 6 — Integration Flow

```rust
#[tokio::main]
async fn main() {
    println!("[Rust] Opening shared memory...");
    let producer = ShmProducer::open("/marketxray_shm").expect("SHM open failed");

    println!("[Rust] Waiting for C++ to be ready...");
    // (C++ sets cpp_ready after wait_for_rust is satisfied)

    producer.signal_ready(); // Tell C++ we are live
    println!("[Rust] Signaled ready. Starting ingestor...");

    // Replace this with your actual NASDAQ ITCH parser
    let mut order_id: u64 = 1;
    loop {
        // Parse your real tick here...
        let tick = Tick::new_add(order_id, 15000, 100, Side::Buy);

        while !producer.push_tick(&tick) {
            // Back-pressure: queue full, spin briefly
            std::hint::spin_loop();
        }

        order_id += 1;
    }
}
```

---

## Verification Checklist

Before running both processes together:

- [ ] `std::mem::size_of::<Tick>() == 64` ✅
- [ ] `std::mem::align_of::<Tick>() == 64` ✅  
- [ ] `SHM_MAGIC` matches in both Rust and C++ (`0x4D585259`) ✅
- [ ] SHM name matches on both sides (default: `/marketxray_shm`) ✅
- [ ] C++ is started **first** (creates the SHM region)
- [ ] Rust is started **second** (opens the existing region)
- [ ] Both processes run on the **same machine**

---

## Run Order

```bash
# Terminal 1 — start C++ daemon (creates SHM)
./market_xray --daemon

# Terminal 2 — start Rust ingestor (opens SHM and starts pushing ticks)
cargo run --release
```
