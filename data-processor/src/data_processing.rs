use anyhow::Result;
use serde::{Deserialize, Serialize};

use crate::device_communication::DataPacket;

/// 处理后的数据（供 WebSocket/文件保存使用）
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessedData {
    pub timestamp: u64,
    pub sequence: u64,
    pub channel_count: usize,
    pub sample_rate: f64,
    pub data: Vec<f64>,
    pub metadata: DataMetadata,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataMetadata {
    pub packet_count: u64,
    pub processing_time_us: u64,
    pub data_quality: DataQuality,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "status", content = "message")]
pub enum DataQuality {
    Good,
    Warning(String),
    Error(String),
}

#[allow(dead_code)]
pub struct DataProcessor {
    // 预留：滤波/标定参数等
}

#[allow(dead_code)]
impl DataProcessor {
    pub fn new() -> Self { Self {} }

    pub async fn run(&mut self) -> Result<()> { Ok(()) }

    /// 将设备上报的数据包转换为可视化友好的结构
    pub fn process_packet(&self, packet: &DataPacket) -> Result<ProcessedData> {
        // 1) int16 LE → 电压（12bit/3.3V 示例）
        let mut samples = Vec::with_capacity(packet.sensor_data.len() / 2);
        for ch in packet.sensor_data.chunks_exact(2) {
            let raw = u16::from_le_bytes([ch[0], ch[1]]);
            let voltage = (raw as f64 / 4095.0) * 3.3;
            samples.push(voltage);
        }

        // 2) 简单 5 点滑动均值
        let filtered = if samples.len() > 5 {
            let mut out = Vec::with_capacity(samples.len());
            for i in 0..samples.len() {
                if i < 2 || i + 2 >= samples.len() {
                    out.push(samples[i]);
                } else {
                    let avg = (samples[i - 2] + samples[i - 1] + samples[i] + samples[i + 1] + samples[i + 2]) / 5.0;
                    out.push(avg);
                }
            }
            out
        } else { samples };

        // 3) 数据质量
        let quality = if filtered.is_empty() {
            DataQuality::Error("No samples".into())
        } else {
            let (min, max) = (
                filtered.iter().cloned().fold(f64::INFINITY, f64::min),
                filtered.iter().cloned().fold(f64::NEG_INFINITY, f64::max),
            );
            if min < -0.1 || max > 3.4 {
                DataQuality::Warning(format!("Out of range: [{:.2}, {:.2}]V", min, max))
            } else { DataQuality::Good }
        };

        Ok(ProcessedData {
            timestamp: packet.timestamp_ms as u64,
            sequence: 0, // 现阶段协议未携带序号，这里填 0；后续可在解析器里补
            channel_count: packet.channel_mask.count_ones() as usize,
            sample_rate: 0.0, // 可由设备信息/配置带入
            data: filtered,
            metadata: DataMetadata {
                packet_count: 0,
                processing_time_us: 0,
                data_quality: quality,
            },
        })
    }
}
