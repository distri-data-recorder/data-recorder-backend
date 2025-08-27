use anyhow::{anyhow, Result};
use bytes::{Buf, BytesMut};
use serde::{Deserialize, Serialize};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::mpsc;
use tokio_serial::{SerialPortBuilderExt, SerialStream};
use tracing::{debug, error, info, warn};

/// 帧常量（如与你设备不同，请同步调整）
const FRAME_HEAD: [u8; 2] = [0xAA, 0x55];
const FRAME_TAIL: [u8; 2] = [0x55, 0xAA];

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceConfig {
    pub connection_type: ConnectionType,
    pub serial_port: Option<String>,
    pub socket_address: Option<String>,
    pub baud_rate: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum ConnectionType {
    Serial,
    Socket,
}

#[derive(Debug, Clone)]
pub struct RawFrame {
    pub command_id: u8,
    pub sequence: u8,
    pub payload: Vec<u8>,
    pub _timestamp: std::time::Instant,
}

#[derive(Debug, Clone)]
pub struct DataPacket {
    pub timestamp_ms: u32,
    pub enabled_channels: u16,  // 修正字段名
    pub sample_count: u16,      // 修正字段名
    pub sensor_data: Vec<u8>,
    pub data_type: DataType,    // 新增：区分数据类型
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum DataType {
    Continuous,
    Trigger {
        trigger_timestamp: u32,
        is_complete: bool,
    },
}

#[derive(Debug, Clone)]
pub struct TriggerEvent {
    pub timestamp: u32,
    pub channel: u16,
    pub pre_samples: u32,
    pub post_samples: u32,
}

#[derive(Debug, Clone)]
pub enum DeviceEvent {
    Connected(String),
    Disconnected,
    FrameReceived(RawFrame),
    DataPacket(DataPacket),
    StatusUpdate(DeviceStatus),
    TriggerEvent(TriggerEvent),        // 新增：触发事件
    BufferTransferComplete,            // 新增：缓冲传输完成
    LogMessage { level: u8, message: String },
    Error(String),
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceStatus {
    pub connected: bool,
    pub device_id: Option<u64>,
    pub firmware_version: Option<u16>,
    pub mode: Option<String>,
    pub stream_active: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChannelConfig {
    pub channel_id: u8,
    pub sample_rate: u32,
    pub format: u8,
}

#[derive(Debug, Clone)]
pub enum DeviceCommand {
    Ping,
    GetDeviceInfo,
    SetModeContinuous,
    SetModeTrigger,
    StartStream,
    StopStream,
    ConfigureStream { channels: Vec<ChannelConfig> },
    RequestBufferedData,               // 新增：请求缓冲数据
}

/// 连接抽象：串口 / TCP（全异步）
pub enum Connection {
    Serial(SerialStream),
    Socket(TcpStream),
}

impl Connection {
    pub async fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        match self {
            Connection::Serial(port) => {
                loop {
                    let n = port.read(buf).await?;
                    if n > 0 {
                        return Ok(n);
                    }
                    tokio::time::sleep(Duration::from_millis(1)).await;
                }
            }
            Connection::Socket(stream) => {
                let n = stream.read(buf).await?;
                if n == 0 {
                    return Err(anyhow!("socket closed"));
                }
                Ok(n)
            }
        }
    }

    pub async fn write(&mut self, data: &[u8]) -> Result<()> {
        match self {
            Connection::Serial(port) => {
                port.write_all(data).await?;
                port.flush().await?;
                Ok(())
            }
            Connection::Socket(stream) => {
                stream.write_all(data).await?;
                stream.flush().await?;
                Ok(())
            }
        }
    }
}

/// 协议解析器
pub struct ProtocolParser {
    buf: BytesMut,
}

impl ProtocolParser {
    pub fn new() -> Self {
        Self { buf: BytesMut::with_capacity(64 * 1024) }
    }

    pub fn feed_data(&mut self, data: &[u8]) -> Result<Vec<RawFrame>> {
        self.buf.extend_from_slice(data);
        self.parse_frames()
    }

    fn parse_frames(&mut self) -> Result<Vec<RawFrame>> {
        let mut frames = Vec::new();
        // 最小帧：2头 +2长 +1cmd +1seq +2crc +2尾 = 10
        while self.buf.len() >= 10 {
            if let Some(frame) = self.try_parse_one()? {
                frames.push(frame);
            } else {
                break;
            }
        }
        Ok(frames)
    }

    fn try_parse_one(&mut self) -> Result<Option<RawFrame>> {
        // 定位帧头
        let start_opt = self.find_frame_start()?;
        let start = match start_opt {
            Some(i) => i,
            None => return Ok(None),
        };
        if start > 0 {
            self.buf.advance(start);
        }
        if self.buf.len() < 10 {
            return Ok(None);
        }
        if self.buf[0] != FRAME_HEAD[0] || self.buf[1] != FRAME_HEAD[1] {
            self.buf.advance(1);
            return Ok(None);
        }

        // 长度：内容长度 = [CMD(1)+SEQ(1)+PAYLOAD(N)+CRC(2)]
        let len = u16::from_le_bytes([self.buf[2], self.buf[3]]) as usize;
        let total = 4 + len + 2; // 头(2)+长(2)+内容(len)+尾(2)
        if self.buf.len() < total {
            return Ok(None);
        }

        // 验尾
        let tail = total - 2;
        if self.buf[tail] != FRAME_TAIL[0] || self.buf[tail + 1] != FRAME_TAIL[1] {
            self.buf.advance(1);
            return Ok(None);
        }

        // 提取字段
        let cmd = self.buf[4];
        let seq = self.buf[5];
        let payload_len = len.saturating_sub(4); // cmd(1)+seq(1)+crc(2)
        let payload_start = 6;
        let payload_end = payload_start + payload_len;

        let mut payload = vec![0u8; payload_len];
        if payload_len > 0 {
            payload.copy_from_slice(&self.buf[payload_start..payload_end]);
        }

        // CRC 覆盖 CMD..PAYLOAD
        let crc_pos = payload_end;
        let rx_crc = u16::from_le_bytes([self.buf[crc_pos], self.buf[crc_pos + 1]]);
        let calc_crc = Self::crc16(&self.buf[4..crc_pos]);
        if rx_crc != calc_crc {
            warn!("CRC mismatch: rx={:04X} calc={:04X}", rx_crc, calc_crc);
            self.buf.advance(1);
            return Ok(None);
        }

        let frame = RawFrame {
            command_id: cmd,
            sequence: seq,
            payload,
            _timestamp: std::time::Instant::now(),
        };
        self.buf.advance(total);
        Ok(Some(frame))
    }

    fn find_frame_start(&self) -> Result<Option<usize>> {
        if self.buf.len() < 2 { return Ok(None); }
        for i in 0..self.buf.len().saturating_sub(1) {
            if self.buf[i] == FRAME_HEAD[0] && self.buf[i + 1] == FRAME_HEAD[1] {
                return Ok(Some(i));
            }
        }
        Ok(None)
    }

    fn crc16(data: &[u8]) -> u16 {
        let mut crc: u16 = 0xFFFF;
        for &b in data {
            crc ^= b as u16;
            for _ in 0..8 {
                if (crc & 1) != 0 {
                    crc = (crc >> 1) ^ 0xA001;
                } else {
                    crc >>= 1;
                }
            }
        }
        crc
    }

    pub fn build_frame(command: u8, seq: u8, payload: &[u8]) -> Vec<u8> {
        // LEN = 1(cmd) + 1(seq) + payload + 2(crc)
        let len = 1 + 1 + payload.len() + 2;
        let mut out = Vec::with_capacity(4 + len + 2);
        out.extend_from_slice(&FRAME_HEAD);
        out.extend_from_slice(&(len as u16).to_le_bytes());
        out.push(command);
        out.push(seq);
        out.extend_from_slice(payload);
        let crc = Self::crc16(&out[4..4 + 2 + payload.len()]);
        out.extend_from_slice(&crc.to_le_bytes());
        out.extend_from_slice(&FRAME_TAIL);
        out
    }
}

/// 设备管理器
pub struct DeviceManager {
    pub config: DeviceConfig,
    connection: Option<Connection>,
    parser: ProtocolParser,
    status: DeviceStatus,

    // 对外事件
    event_tx: mpsc::UnboundedSender<DeviceEvent>,
    // 接收控制命令
    command_rx: mpsc::UnboundedReceiver<DeviceCommand>,

    seq: u8,
    
    // 触发模式状态跟踪
    trigger_active: bool,
    current_trigger: Option<TriggerEvent>,
}

impl DeviceManager {
    pub fn new(config: DeviceConfig)
        -> (Self, mpsc::UnboundedReceiver<DeviceEvent>, mpsc::UnboundedSender<DeviceCommand>)
    {
        let (event_tx, event_rx) = mpsc::unbounded_channel();
        let (cmd_tx, command_rx) = mpsc::unbounded_channel();

        let me = Self {
            config,
            connection: None,
            parser: ProtocolParser::new(),
            status: DeviceStatus {
                connected: false,
                device_id: None,
                firmware_version: None,
                mode: None,
                stream_active: false,
            },
            event_tx,
            command_rx,
            seq: 0,
            trigger_active: false,
            current_trigger: None,
        };
        (me, event_rx, cmd_tx)
    }

    pub async fn run(&mut self) -> Result<()> {
        loop {
            // 连接
            if self.connection.is_none() {
                match self.try_connect().await {
                    Ok(_) => {
                        self.status.connected = true;
                        let _ = self.event_tx.send(DeviceEvent::Connected(format!("{:?}", self.config.connection_type)));
                        // 初始 PING
                        self.send_command(0x01, &[]).await?;
                    }
                    Err(e) => {
                        error!("Connect failed: {}", e);
                        tokio::time::sleep(Duration::from_secs(2)).await;
                        continue;
                    }
                }
            }

            if let Some(conn) = self.connection.as_mut() {
                tokio::select! {
                    // 控制命令
                    Some(cmd) = self.command_rx.recv() => {
                        if let Err(e) = self.handle_command(cmd).await {
                            error!("handle_command: {}", e);
                        }
                    }

                    // 设备读取
                    res = async {
                        let mut buf = [0u8; 4096];
                        conn.read(&mut buf).await.map(|n| (&buf[..n]).to_vec())
                    } => {
                        match res {
                            Ok(bytes) => {
                                if let Err(e) = self.process_bytes(&bytes).await {
                                    error!("process_bytes: {}", e);
                                }
                            }
                            Err(e) => {
                                error!("read error: {}", e);
                                self.connection = None;
                                self.status.connected = false;
                                let _ = self.event_tx.send(DeviceEvent::Disconnected);
                            }
                        }
                    }
                }
            } else {
                tokio::time::sleep(Duration::from_millis(100)).await;
            }
        }
    }

    async fn try_connect(&mut self) -> Result<()> {
        match self.config.connection_type {
            ConnectionType::Serial => {
                let port = self.config.serial_port.as_ref().ok_or_else(|| anyhow!("serial_port not set"))?;
                let baud = self.config.baud_rate;
                let ss = tokio_serial::new(port, baud)
                    .timeout(Duration::from_secs(1))
                    .open_native_async()?;
                info!("Connected serial: {}", port);
                self.connection = Some(Connection::Serial(ss));
            }
            ConnectionType::Socket => {
                let addr = self.config.socket_address.as_ref().ok_or_else(|| anyhow!("socket_address not set"))?;
                let s = TcpStream::connect(addr).await?;
                info!("Connected socket: {}", addr);
                self.connection = Some(Connection::Socket(s));
            }
        }
        Ok(())
    }

    async fn handle_command(&mut self, cmd: DeviceCommand) -> Result<()> {
        match cmd {
            DeviceCommand::Ping => self.send_command(0x01, &[]).await,
            DeviceCommand::GetDeviceInfo => self.send_command(0x03, &[]).await,
            DeviceCommand::SetModeContinuous => {
                self.trigger_active = false;
                self.current_trigger = None;
                self.status.mode = Some("continuous".to_string());
                self.send_command(0x10, &[]).await
            },
            DeviceCommand::SetModeTrigger => {
                self.trigger_active = true;
                self.current_trigger = None;
                self.status.mode = Some("trigger".to_string());
                self.send_command(0x11, &[]).await
            },
            DeviceCommand::StartStream => {
                self.status.stream_active = true;
                self.send_command(0x12, &[]).await
            },
            DeviceCommand::StopStream => {
                self.status.stream_active = false;
                self.send_command(0x13, &[]).await
            },
            DeviceCommand::ConfigureStream { channels } => {
                // 简单示例：数量 + (id, rate, fmt)*
                let mut payload = Vec::with_capacity(1 + channels.len()*7);
                payload.push(channels.len() as u8);
                for ch in channels {
                    payload.push(ch.channel_id);
                    payload.extend_from_slice(&ch.sample_rate.to_le_bytes());
                    payload.push(ch.format);
                }
                self.send_command(0x14, &payload).await
            },
            DeviceCommand::RequestBufferedData => {
                if self.trigger_active {
                    info!("Requesting buffered trigger data");
                    self.send_command(0x42, &[]).await
                } else {
                    warn!("RequestBufferedData called but not in trigger mode");
                    Ok(())
                }
            }
        }
    }

    async fn send_command(&mut self, command: u8, payload: &[u8]) -> Result<()> {
        let seq = self.seq;
        self.seq = self.seq.wrapping_add(1);
        let frame = ProtocolParser::build_frame(command, seq, payload);
        if let Some(conn) = self.connection.as_mut() {
            conn.write(&frame).await?;
            debug!("Sent cmd=0x{:02X} seq={}", command, seq);
            Ok(())
        } else {
            Err(anyhow!("no connection"))
        }
    }

    async fn process_bytes(&mut self, data: &[u8]) -> Result<()> {
        let frames = self.parser.feed_data(data)?;
        for f in frames {
            self.handle_frame(f).await?;
        }
        Ok(())
    }

    async fn handle_frame(&mut self, f: RawFrame) -> Result<()> {
        debug!("frame: cmd=0x{:02X} seq={} len={}", f.command_id, f.sequence, f.payload.len());
        match f.command_id {
            0x81 => { // PONG
                if f.payload.len() >= 8 {
                    let id = u64::from_le_bytes([
                        f.payload[0],f.payload[1],f.payload[2],f.payload[3],
                        f.payload[4],f.payload[5],f.payload[6],f.payload[7],
                    ]);
                    self.status.device_id = Some(id);
                    info!("PONG device_id=0x{:016X}", id);
                }
                let _ = self.event_tx.send(DeviceEvent::StatusUpdate(self.status.clone()));
            }
            0x83 => { // DEVICE_INFO
                if f.payload.len() >= 3 {
                    let fw = u16::from_le_bytes([f.payload[1],f.payload[2]]);
                    self.status.firmware_version = Some(fw);
                    info!("DEVICE_INFO fw={}.{}", fw>>8, fw & 0xFF);
                }
                let _ = self.event_tx.send(DeviceEvent::StatusUpdate(self.status.clone()));
            }
            0x40 => { // DATA_PACKET
                if f.payload.len() >= 8 {
                    let ts = u32::from_le_bytes([f.payload[0],f.payload[1],f.payload[2],f.payload[3]]);
                    let enabled_channels = u16::from_le_bytes([f.payload[4],f.payload[5]]);
                    let sample_count = u16::from_le_bytes([f.payload[6],f.payload[7]]);
                    let data = f.payload[8..].to_vec();
                    
                    // 确定数据类型
                    let data_type = if self.trigger_active && self.current_trigger.is_some() {
                        DataType::Trigger {
                            trigger_timestamp: self.current_trigger.as_ref().unwrap().timestamp,
                            is_complete: false, // 将在传输完成时更新
                        }
                    } else {
                        DataType::Continuous
                    };
                    
                    debug!("DATA packet: ts={}, channels=0x{:04X}, samples={}, type={:?}", 
                           ts, enabled_channels, sample_count, data_type);
                    
                    let pkt = DataPacket {
                        timestamp_ms: ts,
                        enabled_channels,
                        sample_count,
                        sensor_data: data,
                        data_type,
                    };
                    let _ = self.event_tx.send(DeviceEvent::DataPacket(pkt));
                }
            }
            0x41 => { // EVENT_TRIGGERED
                if f.payload.len() >= 14 {
                    let timestamp = u32::from_le_bytes([f.payload[0],f.payload[1],f.payload[2],f.payload[3]]);
                    let channel = u16::from_le_bytes([f.payload[4],f.payload[5]]);
                    let pre_samples = u32::from_le_bytes([f.payload[6],f.payload[7],f.payload[8],f.payload[9]]);
                    let post_samples = u32::from_le_bytes([f.payload[10],f.payload[11],f.payload[12],f.payload[13]]);
                    
                    let trigger_event = TriggerEvent {
                        timestamp,
                        channel,
                        pre_samples,
                        post_samples,
                    };
                    
                    info!("TRIGGER EVENT: ts={}, ch={}, pre={}, post={}", 
                          timestamp, channel, pre_samples, post_samples);
                    
                    // 保存当前触发事件用于后续数据包标识
                    self.current_trigger = Some(trigger_event.clone());
                    let _ = self.event_tx.send(DeviceEvent::TriggerEvent(trigger_event));
                } else {
                    warn!("TRIGGER EVENT with insufficient payload length: {}", f.payload.len());
                }
            }
            0x4F => { // BUFFER_TRANSFER_COMPLETE
                info!("Trigger data transfer complete");
                
                // 更新当前触发的完成状态
                if let Some(ref mut _trigger) = self.current_trigger {
                    // 这里可以进行一些清理工作
                }
                
                let _ = self.event_tx.send(DeviceEvent::BufferTransferComplete);
                
                // 可以选择清除当前触发状态，或者保留用于下一次触发
                // self.current_trigger = None;
            }
            0x90 => { // ACK
                debug!("ACK seq={}", f.sequence);
            }
            0x91 => { // NACK
                warn!("NACK seq={} payload={:X?}", f.sequence, f.payload);
                if f.payload.len() >= 2 {
                    let error_type = f.payload[0];
                    let error_code = f.payload[1];
                    let error_msg = match (error_type, error_code) {
                        (0x01, 0x01) => "Parameter error: invalid parameter".to_string(),
                        (0x01, 0x02) => "Parameter error: invalid channel configuration".to_string(),
                        (0x02, 0x01) => "Status error: invalid mode for operation".to_string(),
                        (0x02, 0x02) => "Status error: trigger not occurred".to_string(),
                        (0x05, 0x00) => "Command not supported".to_string(),
                        _ => format!("Unknown error: type={}, code={}", error_type, error_code),
                    };
                    let _ = self.event_tx.send(DeviceEvent::Error(error_msg));
                }
            }
            0xE0 => { // LOG_MESSAGE
                if f.payload.len() >= 2 {
                    let level = f.payload[0];
                    let msg_len = f.payload[1] as usize;
                    if f.payload.len() >= 2 + msg_len {
                        let message = String::from_utf8_lossy(&f.payload[2..2 + msg_len]).to_string();
                        let _ = self.event_tx.send(DeviceEvent::LogMessage { level, message });
                    }
                }
            }
            _ => {
                debug!("Unknown frame 0x{:02X}", f.command_id);
            }
        }
        let _ = self.event_tx.send(DeviceEvent::FrameReceived(f));
        Ok(())
    }
}