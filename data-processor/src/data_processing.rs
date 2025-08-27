use anyhow::Result;
use serde::{Deserialize, Serialize};

use crate::device_communication::{DataPacket, DataType};

/// 处理后的数据（供 WebSocket/文件保存使用）
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessedData {
    pub timestamp: u64,
    pub sequence: u64,
    pub channel_count: usize,
    pub sample_rate: f64,
    pub data: Vec<f64>,
    pub metadata: DataMetadata,
    pub data_type: ProcessedDataType,  // 新增：数据类型
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessedDataType {
    pub source: DataSource,
    pub trigger_info: Option<TriggerInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum DataSource {
    Continuous,
    Trigger,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TriggerInfo {
    pub trigger_timestamp: u32,
    pub is_complete: bool,
    pub sequence_in_burst: Option<u32>,  // 在触发数据突发中的序号
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataMetadata {
    pub packet_count: u64,
    pub processing_time_us: u64,
    pub data_quality: DataQuality,
    pub channel_info: Vec<ChannelMetadata>,  // 新增：每通道的元数据
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChannelMetadata {
    pub channel_id: u8,
    pub sample_count: usize,
    pub min_value: f64,
    pub max_value: f64,
    pub avg_value: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "status", content = "message")]
pub enum DataQuality {
    Good,
    Warning(String),
    Error(String),
}

pub struct DataProcessor {
    packet_sequence: u64,  // 全局包序号
    trigger_burst_sequence: u32,  // 当前触发突发内的序号
    current_trigger_timestamp: Option<u32>,
}

impl DataProcessor {
    pub fn new() -> Self { 
        Self {
            packet_sequence: 0,
            trigger_burst_sequence: 0,
            current_trigger_timestamp: None,
        }
    }

    pub async fn run(&mut self) -> Result<()> { Ok(()) }

    /// 将设备上报的数据包转换为可视化友好的结构
    pub fn process_packet(&mut self, packet: &DataPacket) -> Result<ProcessedData> {
        let start_time = std::time::Instant::now();
        self.packet_sequence += 1;

        // 1) 解析多通道数据（非交错格式）
        let channel_count = packet.enabled_channels.count_ones() as usize;
        let sample_count = packet.sample_count as usize;
        
        if channel_count == 0 {
            return Err(anyhow::anyhow!("No enabled channels"));
        }

        // 验证数据长度
        let expected_len = channel_count * sample_count * 2; // int16 = 2 bytes
        if packet.sensor_data.len() != expected_len {
            return Err(anyhow::anyhow!(
                "Data length mismatch: expected {}, got {}", 
                expected_len, packet.sensor_data.len()
            ));
        }

        let mut all_samples = Vec::with_capacity(channel_count * sample_count);
        let mut channel_metadata = Vec::new();

        // 按通道处理数据（非交错格式：CH0所有样本，然后CH1所有样本...）
        for ch_idx in 0..channel_count {
            let start_idx = ch_idx * sample_count * 2;
            let end_idx = start_idx + sample_count * 2;
            
            if end_idx <= packet.sensor_data.len() {
                let ch_data = &packet.sensor_data[start_idx..end_idx];
                let mut channel_samples = Vec::with_capacity(sample_count);
                
                for sample_bytes in ch_data.chunks_exact(2) {
                    let raw = i16::from_le_bytes([sample_bytes[0], sample_bytes[1]]);
                    // 转换为电压：int16范围 (-32768 to 32767) 映射到 0-3.3V
                    let voltage = ((raw as f64 + 32768.0) / 65535.0) * 3.3;
                    channel_samples.push(voltage);
                }

                // 计算通道统计信息
                let (min_val, max_val, sum) = channel_samples.iter().fold(
                    (f64::INFINITY, f64::NEG_INFINITY, 0.0),
                    |(min, max, sum), &val| (min.min(val), max.max(val), sum + val)
                );
                
                let channel_id = self.get_channel_id_from_mask(packet.enabled_channels, ch_idx as u8);
                channel_metadata.push(ChannelMetadata {
                    channel_id,
                    sample_count,
                    min_value: min_val,
                    max_value: max_val,
                    avg_value: if sample_count > 0 { sum / sample_count as f64 } else { 0.0 },
                });

                all_samples.extend(channel_samples);
            }
        }

        // 2) 应用滤波处理
        let filtered = self.apply_filter(&all_samples);

        // 3) 数据质量评估
        let quality = self.assess_data_quality(&filtered, &channel_metadata);

        // 4) 处理触发模式状态
        let data_type = match &packet.data_type {
            DataType::Continuous => {
                self.current_trigger_timestamp = None;
                self.trigger_burst_sequence = 0;
                ProcessedDataType {
                    source: DataSource::Continuous,
                    trigger_info: None,
                }
            }
            DataType::Trigger { trigger_timestamp, is_complete } => {
                // 检查是否是新的触发序列
                if self.current_trigger_timestamp != Some(*trigger_timestamp) {
                    self.current_trigger_timestamp = Some(*trigger_timestamp);
                    self.trigger_burst_sequence = 0;
                }
                
                self.trigger_burst_sequence += 1;
                
                let trigger_info = TriggerInfo {
                    trigger_timestamp: *trigger_timestamp,
                    is_complete: *is_complete,
                    sequence_in_burst: Some(self.trigger_burst_sequence),
                };
                
                ProcessedDataType {
                    source: DataSource::Trigger,
                    trigger_info: Some(trigger_info),
                }
            }
        };

        let processing_time = start_time.elapsed().as_micros() as u64;

        Ok(ProcessedData {
            timestamp: packet.timestamp_ms as u64,
            sequence: self.packet_sequence,
            channel_count,
            sample_rate: self.estimate_sample_rate(sample_count, channel_count),
            data: filtered,
            metadata: DataMetadata {
                packet_count: self.packet_sequence,
                processing_time_us: processing_time,
                data_quality: quality,
                channel_info: channel_metadata,
            },
            data_type,
        })
    }

    /// 从通道掩码获取实际通道ID
    fn get_channel_id_from_mask(&self, mask: u16, index: u8) -> u8 {
        let mut current_index = 0;
        for bit in 0..16 {
            if (mask & (1 << bit)) != 0 {
                if current_index == index {
                    return bit;
                }
                current_index += 1;
            }
        }
        index // fallback
    }

    /// 应用5点移动平均滤波
    fn apply_filter(&self, samples: &[f64]) -> Vec<f64> {
        if samples.len() <= 5 {
            return samples.to_vec();
        }

        let mut filtered = Vec::with_capacity(samples.len());
        for i in 0..samples.len() {
            if i < 2 || i + 2 >= samples.len() {
                filtered.push(samples[i]);
            } else {
                let avg = (samples[i-2] + samples[i-1] + samples[i] + samples[i+1] + samples[i+2]) / 5.0;
                filtered.push(avg);
            }
        }
        filtered
    }

    /// 数据质量评估
    fn assess_data_quality(&self, samples: &[f64], channel_info: &[ChannelMetadata]) -> DataQuality {
        if samples.is_empty() {
            return DataQuality::Error("No samples".into());
        }

        let (global_min, global_max) = samples.iter().fold(
            (f64::INFINITY, f64::NEG_INFINITY),
            |(min, max), &val| (min.min(val), max.max(val))
        );

        // 检查电压范围
        if global_min < -0.1 || global_max > 3.4 {
            return DataQuality::Warning(
                format!("Voltage out of range: [{:.2}, {:.2}]V", global_min, global_max)
            );
        }

        // 检查各通道的数据质量
        for ch in channel_info {
            // 检查是否有饱和
            if ch.min_value <= 0.05 || ch.max_value >= 3.25 {
                return DataQuality::Warning(
                    format!("Channel {} near saturation: [{:.2}, {:.2}]V", 
                            ch.channel_id, ch.min_value, ch.max_value)
                );
            }

            // 检查是否有异常平坦的信号
            let range = ch.max_value - ch.min_value;
            if range < 0.001 {
                return DataQuality::Warning(
                    format!("Channel {} signal too flat: range={:.4}V", ch.channel_id, range)
                );
            }
        }

        DataQuality::Good
    }

    /// 估算采样率（基于数据包间隔和样本数量）
    fn estimate_sample_rate(&self, sample_count: usize, channel_count: usize) -> f64 {
        // 假设数据包间隔为10ms（根据C代码中的DATA_SEND_INTERVAL_MS）
        let packet_interval_ms = 10.0;
        let samples_per_channel = sample_count / channel_count.max(1);
        (samples_per_channel as f64 / packet_interval_ms) * 1000.0
    }

    /// 重置触发状态（在模式切换时调用）
    pub fn reset_trigger_state(&mut self) {
        self.current_trigger_timestamp = None;
        self.trigger_burst_sequence = 0;
    }

    /// 获取当前处理统计
    pub fn get_stats(&self) -> ProcessingStats {
        ProcessingStats {
            total_packets_processed: self.packet_sequence,
            current_trigger_burst_sequence: self.trigger_burst_sequence,
            current_trigger_timestamp: self.current_trigger_timestamp,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessingStats {
    pub total_packets_processed: u64,
    pub current_trigger_burst_sequence: u32,
    pub current_trigger_timestamp: Option<u32>,
}