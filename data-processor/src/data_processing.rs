use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use tracing::{info, warn, error};

use crate::device_communication::{DataPacket, DataType, TriggerEvent};

/// 处理后的数据（供 WebSocket/文件保存使用）
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessedData {
    pub timestamp: u64,
    pub sequence: u64,
    pub channel_count: usize,
    pub sample_rate: f64,
    pub data: Vec<f64>,
    pub metadata: DataMetadata,
    pub data_type: ProcessedDataType,
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
    pub sequence_in_burst: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataMetadata {
    pub packet_count: u64,
    pub processing_time_us: u64,
    pub data_quality: DataQuality,
    pub channel_info: Vec<ChannelMetadata>,
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

/// 触发批次数据结构
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TriggerBurst {
    pub burst_id: String,
    pub trigger_timestamp: u32,
    pub trigger_channel: u16,
    pub pre_samples: u32,
    pub post_samples: u32,
    pub data_packets: Vec<ProcessedData>,
    pub is_complete: bool,
    pub total_samples: usize,
    pub created_at: i64,
    pub quality_summary: DataQualitySummary,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataQualitySummary {
    pub overall_quality: DataQuality,
    pub channel_stats: Vec<ChannelStats>,
    pub voltage_range: (f64, f64),
    pub anomaly_count: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChannelStats {
    pub channel_id: u8,
    pub sample_count: usize,
    pub min_value: f64,
    pub max_value: f64,
    pub avg_value: f64,
    pub rms_value: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TriggerSummary {
    pub burst_id: String,
    pub trigger_timestamp: u32,
    pub trigger_channel: u16,
    pub total_samples: usize,
    pub duration_ms: f64,
    pub created_at: i64,
    pub quality: String,
    pub can_save: bool,
}

pub struct DataProcessor {
    packet_sequence: u64,
    trigger_burst_sequence: u32,
    current_trigger_timestamp: Option<u32>,
    
    // 触发批次管理
    current_trigger_burst: Option<TriggerBurst>,
    completed_trigger_bursts: HashMap<String, TriggerBurst>,
    max_cached_bursts: usize,
}

impl DataProcessor {
    pub fn new() -> Self { 
        Self {
            packet_sequence: 0,
            trigger_burst_sequence: 0,
            current_trigger_timestamp: None,
            current_trigger_burst: None,
            completed_trigger_bursts: HashMap::new(),
            max_cached_bursts: 10,
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

        let processed = ProcessedData {
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
        };

        // 如果是触发数据，添加到当前批次
        if let DataType::Trigger { .. } = &packet.data_type {
            if let Some(ref mut burst) = self.current_trigger_burst {
                burst.data_packets.push(processed.clone());
                burst.total_samples += processed.data.len();
            }
        }

        Ok(processed)
    }

    /// 开始新的触发批次
    pub fn start_trigger_burst(&mut self, trigger_event: &TriggerEvent) -> String {
        let burst_id = format!("trigger_{}_{}", 
                              trigger_event.timestamp, 
                              chrono::Utc::now().timestamp_millis());
        
        self.current_trigger_burst = Some(TriggerBurst {
            burst_id: burst_id.clone(),
            trigger_timestamp: trigger_event.timestamp,
            trigger_channel: trigger_event.channel,
            pre_samples: trigger_event.pre_samples,
            post_samples: trigger_event.post_samples,
            data_packets: Vec::new(),
            is_complete: false,
            total_samples: 0,
            created_at: chrono::Utc::now().timestamp_millis(),
            quality_summary: DataQualitySummary {
                overall_quality: DataQuality::Good,
                channel_stats: Vec::new(),
                voltage_range: (f64::INFINITY, f64::NEG_INFINITY),
                anomaly_count: 0,
            },
        });
        
        info!("Started new trigger burst: {}", burst_id);
        burst_id
    }

    /// 完成当前触发批次
    pub fn complete_trigger_burst(&mut self) -> Option<TriggerBurst> {
        if let Some(mut burst) = self.current_trigger_burst.take() {
            burst.is_complete = true;
            
            // 计算质量摘要
            self.calculate_quality_summary(&mut burst);
            
            // 添加到完成列表
            let burst_id = burst.burst_id.clone();
            self.completed_trigger_bursts.insert(burst_id, burst.clone());
            
            // 限制缓存数量（保留最新的）
            if self.completed_trigger_bursts.len() > self.max_cached_bursts {
                let mut timestamps: Vec<_> = self.completed_trigger_bursts.values()
                    .map(|b| (b.created_at, b.burst_id.clone()))
                    .collect();
                timestamps.sort_by_key(|&(ts, _)| ts);
                
                // 删除最旧的
                let oldest_id = &timestamps[0].1;
                self.completed_trigger_bursts.remove(oldest_id);
            }
            
            info!("Completed trigger burst: {} with {} packets", 
                  burst.burst_id, burst.data_packets.len());
            Some(burst)
        } else {
            None
        }
    }

    /// 获取触发批次摘要列表
    pub fn get_trigger_summaries(&self) -> Vec<TriggerSummary> {
        let mut summaries: Vec<_> = self.completed_trigger_bursts.values()
            .map(|burst| TriggerSummary {
                burst_id: burst.burst_id.clone(),
                trigger_timestamp: burst.trigger_timestamp,
                trigger_channel: burst.trigger_channel,
                total_samples: burst.total_samples,
                duration_ms: self.calculate_duration_ms(burst),
                created_at: burst.created_at,
                quality: match burst.quality_summary.overall_quality {
                    DataQuality::Good => "Good".to_string(),
                    DataQuality::Warning(_) => "Warning".to_string(),
                    DataQuality::Error(_) => "Error".to_string(),
                },
                can_save: burst.is_complete && !burst.data_packets.is_empty(),
            })
            .collect();
        
        // 按创建时间倒序排列
        summaries.sort_by(|a, b| b.created_at.cmp(&a.created_at));
        summaries
    }

    /// 获取指定触发批次的详细数据
    pub fn get_trigger_burst(&self, burst_id: &str) -> Option<&TriggerBurst> {
        self.completed_trigger_bursts.get(burst_id)
    }

    /// 删除指定的触发批次
    pub fn remove_trigger_burst(&mut self, burst_id: &str) -> bool {
        self.completed_trigger_bursts.remove(burst_id).is_some()
    }

    /// 导出触发批次为保存格式
    pub fn export_trigger_burst(&self, burst_id: &str, format: &str) -> Result<Vec<u8>> {
        let burst = self.get_trigger_burst(burst_id)
            .ok_or_else(|| anyhow::anyhow!("Trigger burst not found: {}", burst_id))?;

        match format {
            "json" => {
                let json = serde_json::to_string_pretty(burst)?;
                Ok(json.into_bytes())
            }
            "csv" => {
                self.export_burst_as_csv(burst)
            }
            "binary" => {
                self.export_burst_as_binary(burst)
            }
            _ => Err(anyhow::anyhow!("Unsupported format: {}", format))
        }
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

    /// 计算质量摘要
    fn calculate_quality_summary(&self, burst: &mut TriggerBurst) {
        // 计算全局统计信息
        let mut all_samples = Vec::new();
        let mut channel_data: HashMap<u8, Vec<f64>> = HashMap::new();

        for packet in &burst.data_packets {
            all_samples.extend(&packet.data);
            
            // 按通道分组数据（简化处理）
            for (i, &sample) in packet.data.iter().enumerate() {
                let channel_id = (i % packet.channel_count) as u8;
                channel_data.entry(channel_id).or_default().push(sample);
            }
        }

        // 计算电压范围
        if !all_samples.is_empty() {
            let min_val = all_samples.iter().fold(f64::INFINITY, |a, &b| a.min(b));
            let max_val = all_samples.iter().fold(f64::NEG_INFINITY, |a, &b| a.max(b));
            burst.quality_summary.voltage_range = (min_val, max_val);
        }

        // 计算各通道统计信息
        burst.quality_summary.channel_stats = channel_data.iter()
            .map(|(&channel_id, samples)| {
                let sum: f64 = samples.iter().sum();
                let count = samples.len();
                let avg = if count > 0 { sum / count as f64 } else { 0.0 };
                let min_val = samples.iter().fold(f64::INFINITY, |a, &b| a.min(b));
                let max_val = samples.iter().fold(f64::NEG_INFINITY, |a, &b| a.max(b));
                
                // 计算RMS值
                let sum_squares: f64 = samples.iter().map(|&x| x * x).sum();
                let rms = if count > 0 { (sum_squares / count as f64).sqrt() } else { 0.0 };

                ChannelStats {
                    channel_id,
                    sample_count: count,
                    min_value: min_val,
                    max_value: max_val,
                    avg_value: avg,
                    rms_value: rms,
                }
            })
            .collect();

        // 评估整体质量
        burst.quality_summary.overall_quality = self.assess_burst_quality(burst);
    }

    fn assess_burst_quality(&self, burst: &TriggerBurst) -> DataQuality {
        let (min_val, max_val) = burst.quality_summary.voltage_range;
        
        // 检查电压范围
        if min_val < -0.1 || max_val > 3.4 {
            return DataQuality::Warning(
                format!("Voltage out of range: [{:.2}, {:.2}]V", min_val, max_val)
            );
        }

        // 检查数据完整性
        if !burst.is_complete {
            return DataQuality::Warning("Trigger data incomplete".to_string());
        }

        if burst.data_packets.is_empty() {
            return DataQuality::Error("No data packets in trigger burst".to_string());
        }

        DataQuality::Good
    }

    pub fn calculate_duration_ms(&self, burst: &TriggerBurst) -> f64 {
        if burst.data_packets.len() <= 1 {
            return 0.0;
        }

        let first_ts = burst.data_packets[0].timestamp;
        let last_ts = burst.data_packets.last().unwrap().timestamp;
        (last_ts - first_ts) as f64
    }

    fn export_burst_as_csv(&self, burst: &TriggerBurst) -> Result<Vec<u8>> {
        let mut csv_content = String::new();
        
        // CSV头部
        csv_content.push_str("timestamp_ms,channel_id,sample_index,value\n");
        
        // 数据行
        for packet in &burst.data_packets {
            let samples_per_channel = packet.data.len() / packet.channel_count.max(1);
            
            for ch in 0..packet.channel_count {
                for sample_idx in 0..samples_per_channel {
                    let data_idx = ch * samples_per_channel + sample_idx;
                    if data_idx < packet.data.len() {
                        csv_content.push_str(&format!(
                            "{},{},{},{:.6}\n",
                            packet.timestamp,
                            ch,
                            sample_idx,
                            packet.data[data_idx]
                        ));
                    }
                }
            }
        }
        
        Ok(csv_content.into_bytes())
    }

    fn export_burst_as_binary(&self, burst: &TriggerBurst) -> Result<Vec<u8>> {
        // 简单的二进制格式：
        // [8字节头] [4字节样本数] [样本数据...]
        let mut binary_data = Vec::new();
        
        // 写入头部信息
        binary_data.extend(&burst.trigger_timestamp.to_le_bytes());
        binary_data.extend(&(burst.trigger_channel as u32).to_le_bytes());
        
        // 写入样本数
        binary_data.extend(&(burst.total_samples as u32).to_le_bytes());
        
        // 写入样本数据（32位浮点数）
        for packet in &burst.data_packets {
            for &sample in &packet.data {
                binary_data.extend(&(sample as f32).to_le_bytes());
            }
        }
        
        Ok(binary_data)
    }

    /// 重置触发状态（在模式切换时调用）
    pub fn reset_trigger_state(&mut self) {
        self.current_trigger_timestamp = None;
        self.trigger_burst_sequence = 0;
        self.current_trigger_burst = None;
    }

    /// 获取当前处理统计
    pub fn get_stats(&self) -> ProcessingStats {
        ProcessingStats {
            total_packets_processed: self.packet_sequence,
            current_trigger_burst_sequence: self.trigger_burst_sequence,
            current_trigger_timestamp: self.current_trigger_timestamp,
            cached_bursts_count: self.completed_trigger_bursts.len(),
            current_burst_active: self.current_trigger_burst.is_some(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessingStats {
    pub total_packets_processed: u64,
    pub current_trigger_burst_sequence: u32,
    pub current_trigger_timestamp: Option<u32>,
    pub cached_bursts_count: usize,
    pub current_burst_active: bool,
}