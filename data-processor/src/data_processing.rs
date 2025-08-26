use crate::ipc::{ADCDataPacket, SharedMemoryReader};
use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::sync::Arc;
use tokio::sync::{broadcast, RwLock};
use tokio::time::{interval, Duration};
use tracing::{debug, error, info, warn};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessedData {
    pub timestamp: u64,
    pub sequence: u16,
    pub channel_count: usize,
    pub sample_rate: f64,
    pub data: Vec<f64>,
    pub metadata: DataMetadata,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataMetadata {
    pub packet_count: u32,
    pub processing_time_us: u64,
    pub data_quality: DataQuality,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum DataQuality {
    Good,
    Warning(String),
    Error(String),
}

pub struct DataProcessor {
    shared_mem: Arc<RwLock<SharedMemoryReader>>,
    processed_data_tx: broadcast::Sender<ProcessedData>,
    buffer: VecDeque<ADCDataPacket>,
    packet_count: u64,
}

impl DataProcessor {
    pub fn new(shared_mem: SharedMemoryReader) -> Self {
        let (tx, _) = broadcast::channel(1000);
        Self {
            shared_mem: Arc::new(RwLock::new(shared_mem)),
            processed_data_tx: tx,
            buffer: VecDeque::with_capacity(1024),
            packet_count: 0,
        }
    }

    pub fn get_data_receiver(&self) -> broadcast::Receiver<ProcessedData> {
        self.processed_data_tx.subscribe()
    }

    pub async fn run(&mut self) -> Result<()> {
        info!("Starting data processing loop");
        // 按配置可调（此处用默认 10ms），与 main/config 对齐
        let mut tick = interval(Duration::from_millis(10));

        loop {
            tick.tick().await;

            // 读取新的数据包（批量）
            let new_packets = {
                let mut reader = self.shared_mem.write().await;
                reader.read_new_packets()?
            };

            if !new_packets.is_empty() {
                debug!("Received {} new packets", new_packets.len());

                for packet in new_packets {
                    self.buffer.push_back(packet);
                    self.packet_count += 1;
                }

                self.process_buffered_data().await?;
            }

            self.cleanup_old_data();
        }
    }

    async fn process_buffered_data(&mut self) -> Result<()> {
        while let Some(packet) = self.buffer.pop_front() {
            let start_time = std::time::Instant::now();

            match self.process_single_packet(&packet).await {
                Ok(mut processed) => {
                    processed.metadata.processing_time_us =
                        start_time.elapsed().as_micros() as u64;

                    if let Err(e) = self.processed_data_tx.send(processed) {
                        warn!("Failed to broadcast processed data: {}", e);
                    }
                }
                Err(e) => {
                    error!(
                        "Failed to process packet seq={} len={}: {}",
                        packet.sequence, packet.payload_len, e
                    );
                }
            }
        }
        Ok(())
    }

    async fn process_single_packet(&self, packet: &ADCDataPacket) -> Result<ProcessedData> {
        // 从固定数组中按 payload_len 截断有效数据
        let payload_len = packet.payload_len as usize;
        let payload = &packet.payload[..payload_len.min(packet.payload.len())];

        // 假设单通道 16bit little-endian 原始样本
        let mut samples = Vec::with_capacity(payload.len() / 2);
        for ch in payload.chunks_exact(2) {
            let raw = u16::from_le_bytes([ch[0], ch[1]]);
            // 12bit ADC 映射 0..4095 -> 0..3.3V（如需双极性可调整）
            let volt = (raw as f64 / 4095.0) * 3.3;
            samples.push(volt);
        }

        // 质量评估
        let quality = self.assess_data_quality(&samples);

        // 简单移动平均（窗口=5）
        let filtered = self.apply_moving_average(&samples, 5);

        Ok(ProcessedData {
            timestamp: packet.timestamp_ms as u64,
            sequence: packet.sequence,
            channel_count: 1,
            sample_rate: 1000.0, // 若有真实速率，可从配置/协议里带出
            data: filtered,
            metadata: DataMetadata {
                packet_count: self.packet_count as u32,
                processing_time_us: 0,
                data_quality: quality,
            },
        })
    }

    fn assess_data_quality(&self, samples: &[f64]) -> DataQuality {
        if samples.is_empty() {
            return DataQuality::Error("No data samples".into());
        }
        // 简单规则：电压必须在 0..3.3V 内，否则警告
        let mut min_v = f64::INFINITY;
        let mut max_v = f64::NEG_INFINITY;
        for &v in samples {
            if v < min_v {
                min_v = v;
            }
            if v > max_v {
                max_v = v;
            }
        }
        if min_v < -0.1 || max_v > 3.4 {
            DataQuality::Warning(format!("Out-of-range: [{:.3}, {:.3}]", min_v, max_v))
        } else {
            DataQuality::Good
        }
    }

    fn apply_moving_average(&self, data: &[f64], win: usize) -> Vec<f64> {
        if win <= 1 || data.len() < win {
            return data.to_vec();
        }
        let mut out = Vec::with_capacity(data.len());
        let mut acc = 0.0;
        for i in 0..data.len() {
            acc += data[i];
            if i >= win {
                acc -= data[i - win];
            }
            if i + 1 >= win {
                out.push(acc / win as f64);
            } else {
                out.push(data[i]);
            }
        }
        out
    }

    fn cleanup_old_data(&mut self) {
        // 这里暂时无需要清理的过期数据；若引入时间窗口缓存可在此清理
    }
}
