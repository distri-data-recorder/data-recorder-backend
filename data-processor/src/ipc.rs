use anyhow::{Result, anyhow};
use std::sync::atomic::{AtomicU32, Ordering};

// 共享内存数据结构（与C代码保持一致）
#[repr(C)]
#[derive(Debug)]
pub struct SharedMemHeader {
    pub magic: u32,           // 0xADC12345
    pub version: u32,         // 1
    pub write_index: AtomicU32,
    pub read_index: AtomicU32,
    pub buffer_size: u32,     // 1024
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

// 消息队列数据结构
#[repr(C)]
#[derive(Debug, Clone)]
pub enum MessageType {
    StartCollection = 1,
    StopCollection = 2,
    StatusRequest = 3,
    StatusResponse = 4,
    SaveWaveform = 5,
    Error = 99,
}

#[repr(C)]
#[derive(Debug, Clone)]
pub struct IPCMessage {
    pub msg_type: u32,
    pub timestamp: u32,
    pub data_len: u32,
    pub data: [u8; 256],
}

pub struct SharedMemoryReader {
    _mapping: shared_memory::Shmem,
    shared_mem: *mut SharedMemory,
    last_read_index: u32,
}

unsafe impl Send for SharedMemoryReader {}
unsafe impl Sync for SharedMemoryReader {}

impl SharedMemoryReader {
    pub fn new(name: &str) -> Result<Self> {
        // 尝试打开现有的共享内存
        let mapping = shared_memory::ShmemConf::new()
            .size(std::mem::size_of::<SharedMemory>())
            .flink(name)
            .open()
            .map_err(|e| anyhow!("Failed to open shared memory '{}': {}", name, e))?;

        let shared_mem = mapping.as_ptr() as *mut SharedMemory;

        // 验证魔数和版本
        unsafe {
            let header = &(*shared_mem).header;
            if header.magic != 0xADC12345 {
                return Err(anyhow!("Invalid magic number in shared memory"));
            }
            if header.version != 1 {
                return Err(anyhow!("Unsupported shared memory version"));
            }
        }

        Ok(Self {
            _mapping: mapping,
            shared_mem,
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

            // 更新读取索引
            header.read_index.store(self.last_read_index, Ordering::Release);
        }

        Ok(packets)
    }

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

pub struct MessageQueue {
    // 占位符 - 实际实现将使用Windows命名管道或其他IPC机制
    _name: String,
}

impl MessageQueue {
    pub fn new(name: &str) -> Result<Self> {
        // TODO: 实现实际的消息队列连接
        Ok(Self {
            _name: name.to_string(),
        })
    }

    pub async fn send_message(&self, _msg: IPCMessage) -> Result<()> {
        // TODO: 实现消息发送
        Ok(())
    }

    pub async fn receive_message(&self) -> Result<Option<IPCMessage>> {
        // TODO: 实现消息接收
        Ok(None)
    }
}