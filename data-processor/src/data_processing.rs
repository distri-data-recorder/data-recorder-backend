use crate::ipc::{SharedMemoryReader, ADCDataPacket};
use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::sync::Arc;
use tokio::sync::{broadcast, RwLock};
use tokio::time::{interval, Duration};
use tracing::{info, warn, error, debug};

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

        let mut interval = interval(Duration::from_millis(10));

        loop {
            interval.tick().await;

            // 读取新的数据包
            let new_packets = {
                let mut reader = self.shared_mem.write().await;
                reader.read_new_packets()?
            };

            if !new_packets.is_empty() {
                debug!("Received {} new packets", new_packets.len());

                // 添加到缓冲区
                for packet in new_packets {
                    self.buffer.push_back(packet);
                    self.packet_count += 1;
                }

                // 处理缓冲区中的数据
                self.process_buffered_data().await?;
            }

            // 清理过期数据
            self.cleanup_old_data();
        }
    }

    async fn process_buffered_data(&mut self) -> Result<()> {
        while let Some(packet) = self.buffer.pop_front() {
            let start_time = std::time::Instant::now();

            match self.process_single_packet(&packet).await {
                Ok(processed) => {
                    let processing_time = start_time.elapsed().as_micros() as u64;

                    let mut processed_with_timing = processed;
                    processed_with_timing.metadata.processing_time_us = processing_time;

                    // 发送处理后的数据
                    if let Err(e) = self.processed_data_tx.send(processed_with_timing) {
                        warn!("Failed to send processed data: {}", e);
                    }
                }
                Err(e) => {
                    error!("Failed to process packet {}: {}", packet.sequence, e);
                }
            }
        }

        Ok(())
    }

    async fn process_single_packet(&self, packet: &ADCDataPacket) -> Result<ProcessedData> {
        // 基本的数据解析和处理
        let payload = &packet.payload[..packet.payload_len as usize];

        // 假设ADC数据是16位整数，小端序
        let mut samples = Vec::new();
        for chunk in payload.chunks_exact(2) {
            if chunk.len() == 2 {
                let raw_value = u16::from_le_bytes([chunk[0], chunk[1]]);
                // 转换为电压值（假设3.3V参考电压，12位ADC）
                let voltage = (raw_value as f64 / 4095.0) * 3.3;
                samples.push(voltage);
            }
        }

        // 数据质量检查
        let quality = self.assess_data_quality(&samples);

        // 应用简单的滤波（移动平均）
        let filtered_samples = self.apply_moving_average(&samples, 5);

        Ok(ProcessedData {
            timestamp: packet.timestamp_ms as u64,
            sequence: packet.sequence,
            channel_count: 1, // 假设单通道
            sample_rate: 1000.0, // 假设1kHz采样率
            data: filtered_samples,
            metadata: DataMetadata {
                packet_count: self.packet_count as u32,
                processing_time_us: 0, // 将在调用处设置
                data_quality: quality,
            },
        })
    }

    fn assess_data_quality(&self, samples: &[f64]) -> DataQuality {
        if samples.is_empty() {
            return DataQuality::Error("No data samples".to_string());
        }

        // 检查数据范围
        let min_val = samples.iter().fold(f64::INFINITY, |a, &b| a.min(b));
        let max_val = samples.iter().fold(f64::NEG_INFINITY, |a, &b| a.max(b));

        if min_val < 0.0 || max_val > 3.3 {
            return DataQuality::Warning("Data out of expected range".to_string());
        }

        // 检查数据变化
        let mean = samples.iter().sum::<f64>() / samples.len() as f64;
        let variance = samples.iter()
            .map(|x| (x - mean).powi(2))
            .sum::<f64>() / samples.len() as f64;

        if variance < 1e-6 {
            return DataQuality::Warning("Data appears to be constant".to_string());
        }

        DataQuality::Good
    }

    fn apply_moving_average(&self, samples: &[f64], window_size: usize) -> Vec<f64> {
        if samples.len() < window_size {
            return samples.to_vec();
        }

        let mut filtered = Vec::with_capacity(samples.len());

        for i in 0..samples.len() {
            let start = if i >= window_size / 2 { i - window_size / 2 } else { 0 };
            let end = std::cmp::min(start + window_size, samples.len());

            let sum: f64 = samples[start..end].iter().sum();
            let avg = sum / (end - start) as f64;
            filtered.push(avg);
        }

        filtered
    }

    fn cleanup_old_data(&mut self) {
        // 保持缓冲区大小在合理范围内
        const MAX_BUFFER_SIZE: usize = 100;

        while self.buffer.len() > MAX_BUFFER_SIZE {
            self.buffer.pop_front();
        }
    }
}