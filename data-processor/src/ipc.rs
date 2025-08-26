use anyhow::{anyhow, Result};
use serde_json::Value as JsonValue;
use serde_json::json;
use std::ffi::CString;
use std::io::{BufRead, BufReader, Write};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;
use tokio::sync::{broadcast, mpsc};
use tracing::info;

// ==== Win32 FFI：与 data-reader 对齐 ====
use core::ffi::c_void;
use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
use windows_sys::Win32::System::Memory::{
    MapViewOfFile, OpenFileMappingA, UnmapViewOfFile, FILE_MAP_READ, FILE_MAP_WRITE,
    MEMORY_MAPPED_VIEW_ADDRESS,
};

/// ========================= 共享内存布局 =========================

#[repr(C)]
#[derive(Debug)]
pub struct SharedMemHeader {
    pub magic: u32, // 0xADC12345
    pub version: u32,
    pub write_index: AtomicU32,
    pub read_index: AtomicU32,
    pub buffer_size: u32, // 1024
    pub packet_count: AtomicU32,
    pub status: u8,
    pub reserved: [u8; 7],
}

#[repr(C)]
#[derive(Debug, Clone)]
pub struct ADCDataPacket {
    pub timestamp_ms: u32,
    pub sequence: u16,
    pub payload_len: u16,
    pub payload: [u8; 4096],
}

#[repr(C)]
pub struct SharedMemory {
    pub header: SharedMemHeader,
    pub packets: [ADCDataPacket; 1024],
}

pub struct SharedMemoryReader {
    h_map: HANDLE,
    shared_mem: *mut SharedMemory,
    last_read_index: u32,
}

unsafe impl Send for SharedMemoryReader {}
unsafe impl Sync for SharedMemoryReader {}

impl Drop for SharedMemoryReader {
    fn drop(&mut self) {
        unsafe {
            if !self.shared_mem.is_null() {
                // windows-sys 里 UnmapViewOfFile 接收 MEMORY_MAPPED_VIEW_ADDRESS
                let addr = MEMORY_MAPPED_VIEW_ADDRESS {
                    Value: self.shared_mem as *mut c_void,
                };
                UnmapViewOfFile(addr);
            }
            if self.h_map != 0 {
                CloseHandle(self.h_map);
            }
        }
    }
}

impl SharedMemoryReader {
    pub fn new(name: &str) -> Result<Self> {
        // 依次尝试：裸名 / Local\ / Global\
        let candidates = [
            name.to_string(),
            format!("Local\\{}", name),
            format!("Global\\{}", name),
        ];

        let (h_map, view_ptr, used_name) = {
            let mut last_err: Option<anyhow::Error> = None;
            let mut got: Option<(HANDLE, *mut SharedMemory, String)> = None;

            for nm in &candidates {
                let c_name = CString::new(nm.as_str()).unwrap();
                unsafe {
                    let h = OpenFileMappingA(
                        (FILE_MAP_READ | FILE_MAP_WRITE) as u32,
                        0, // 不继承
                        c_name.as_ptr() as *const u8,
                    );
                    if h == 0 {
                        last_err = Some(anyhow!("OpenFileMappingA({}) failed", nm));
                        continue;
                    }
                    let view: MEMORY_MAPPED_VIEW_ADDRESS =
                        MapViewOfFile(h, (FILE_MAP_READ | FILE_MAP_WRITE) as u32, 0, 0, 0);
                    if view.Value.is_null() {
                        CloseHandle(h);
                        last_err = Some(anyhow!("MapViewOfFile({}) failed", nm));
                        continue;
                    }
                    let ptr = view.Value as *mut SharedMemory;
                    got = Some((h, ptr, nm.clone()));
                }
                break;
            }

            if let Some(x) = got {
                x
            } else {
                return Err(anyhow!(
                    "Failed to open shared memory '{}'. Tried [{}]. Last error: {}",
                    name,
                    candidates.join(", "),
                    last_err.map(|e| e.to_string()).unwrap_or_default()
                ));
            }
        };

        info!("Shared memory opened with name '{}'", used_name);

        unsafe {
            let header = &(*view_ptr).header;
            if header.magic != 0xADC12345 {
                return Err(anyhow!(
                    "Invalid magic number in shared memory: 0x{:08X}",
                    header.magic
                ));
            }
            if header.version != 1 {
                return Err(anyhow!("Unsupported shared memory version: {}", header.version));
            }
        }

        Ok(Self {
            h_map,
            shared_mem: view_ptr,
            last_read_index: 0,
        })
    }

    pub fn read_new_packets(&mut self) -> Result<Vec<ADCDataPacket>> {
        let mut packets = Vec::new();

        unsafe {
            let header = &(*self.shared_mem).header;
            let current_write_index = header.write_index.load(Ordering::Acquire);

            while self.last_read_index != current_write_index {
                let packet_index = (self.last_read_index % header.buffer_size) as usize;
                let packet = (*self.shared_mem).packets[packet_index].clone();
                packets.push(packet);

                self.last_read_index = (self.last_read_index + 1) % header.buffer_size;
            }

            header
                .read_index
                .store(self.last_read_index, Ordering::Release);
        }

        Ok(packets)
    }

    #[allow(dead_code)]
    pub fn get_status(&self) -> Result<(u32, u32, u32)> {
        unsafe {
            let header = &(*self.shared_mem).header;
            Ok((
                header.write_index.load(Ordering::Acquire),
                header.read_index.load(Ordering::Acquire),
                header.packet_count.load(Ordering::Acquire),
            ))
        }
    }
}

/// ========================= 命名管道 JSON-Lines 客户端 =========================

pub struct IpcClient {
    _pipe_name: String,
    send_tx: mpsc::UnboundedSender<String>,
    _send_task: thread::JoinHandle<()>,
    _recv_task: thread::JoinHandle<()>,
    incoming_tx: broadcast::Sender<JsonValue>,
}

impl IpcClient {
    pub fn start(pipe_name: &str) -> Result<Arc<Self>> {
        let (send_tx, mut send_rx) = mpsc::unbounded_channel::<String>();
        let (incoming_tx, _) = broadcast::channel::<JsonValue>(1024);
        let pipe = pipe_name.to_string();

        // 发送线程
        let pipe_w = pipe.clone();
        let incoming_tx_w = incoming_tx.clone();
        let send_task = thread::spawn(move || {
            loop {
                let mut file = match std::fs::OpenOptions::new()
                    .write(true)
                    .read(false)
                    .open(&pipe_w)
                {
                    Ok(f) => f,
                    Err(_) => {
                        thread::sleep(Duration::from_millis(200));
                        continue;
                    }
                };

                loop {
                    match send_rx.blocking_recv() {
                        Some(mut line) => {
                            if !line.ends_with('\n') {
                                line.push('\n');
                            }
                            if let Err(e) = file.write_all(line.as_bytes()) {
                                let _ = file.flush();
                                let _ = incoming_tx_w.send(json!({
                                    "type": "ipc_warning",
                                    "message": format!("send failed: {}", e)
                                }));
                                break;
                            }
                            let _ = file.flush();
                        }
                        None => return,
                    }
                }
            }
        });

        // 接收线程
        let pipe_r = pipe.clone();
        let incoming_tx_r = incoming_tx.clone();
        let recv_task = thread::spawn(move || loop {
            let file = match std::fs::OpenOptions::new().read(true).write(false).open(&pipe_r) {
                Ok(f) => f,
                Err(_) => {
                    thread::sleep(Duration::from_millis(200));
                    continue;
                }
            };

            let mut reader = BufReader::new(file);
            let mut buf = String::new();

            loop {
                buf.clear();
                match reader.read_line(&mut buf) {
                    Ok(0) => break, // 断开
                    Ok(_) => {
                        if let Ok(v) = serde_json::from_str::<JsonValue>(buf.trim_end()) {
                            let _ = incoming_tx_r.send(v);
                        } else {
                            let _ = incoming_tx_r.send(json!({
                                "type": "ipc_parse_error",
                                "raw": buf.clone(),
                            }));
                        }
                    }
                    Err(_) => break,
                }
            }
        });

        Ok(Arc::new(Self {
            _pipe_name: pipe,
            send_tx,
            _send_task: send_task,
            _recv_task: recv_task,
            incoming_tx,
        }))
    }

    pub fn send_json(&self, v: &JsonValue) -> Result<()> {
        let line = serde_json::to_string(v)?;
        self.send_tx
            .send(line)
            .map_err(|e| anyhow!("send channel error: {}", e))
    }

    pub fn subscribe(&self) -> broadcast::Receiver<JsonValue> {
        self.incoming_tx.subscribe()
    }
}
