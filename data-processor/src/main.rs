mod device_communication;
mod data_processing;
mod web_server;
mod websocket;
mod file_manager;
mod config;

use anyhow::Result;
use tokio::sync::watch;
use tracing::{error, info, warn};
use crate::data_processing::{ProcessedData, DataMetadata, DataQuality};
use device_communication::{DeviceManager, DeviceConfig, ConnectionType, DeviceEvent, DataPacket};

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt::init();
    info!("Starting Integrated Data Processor v2.0");

    // 加载配置
    let cfg = config::Config::load()?;
    info!("Configuration loaded");
    info!("Device: {} mode", cfg.device.connection_type);
    info!("WebSocket: {}:{}", cfg.websocket.host, cfg.websocket.port);
    info!("HTTP API: {}:{}", cfg.web_server.host, cfg.web_server.port);

    // 转换设备配置
    let device_config = DeviceConfig {
        connection_type: match cfg.device.connection_type.as_str() {
            "serial" => ConnectionType::Serial,
            _ => ConnectionType::Socket,
        },
        serial_port: cfg.device.serial_port.clone(),
        socket_address: cfg.device.socket_address.clone(),
        baud_rate: cfg.device.baud_rate,
    };

    // 创建设备管理器
    let (mut device_manager, mut device_events, device_command_tx) = DeviceManager::new(device_config);

    // 用于统计处理的数据包数量
    let (pkt_tx, pkt_rx) = watch::channel(0u64);
    
    // 用于广播处理后的数据到WebSocket
    let (processed_tx, processed_rx_for_ws) = tokio::sync::broadcast::channel(1000);

    // ======= 设备管理任务 =======
    let device_handle = tokio::spawn(async move {
        loop {
            match device_manager.run().await {
                Ok(_) => {
                    warn!("Device manager exited normally, restarting...");
                }
                Err(e) => {
                    error!("Device manager error: {}, restarting in 5s...", e);
                    tokio::time::sleep(tokio::time::Duration::from_secs(5)).await;
                }
            }
        }
    });

    // ======= 设备事件处理任务 =======
    let processed_tx_clone = processed_tx.clone();
    let pkt_tx_clone = pkt_tx.clone();
    let event_handle = tokio::spawn(async move {
        let mut packet_count = 0u64;
        
        while let Some(event) = device_events.recv().await {
            match event {
                DeviceEvent::Connected(conn_type) => {
                    info!("Device connected: {}", conn_type);
                }
                DeviceEvent::Disconnected => {
                    warn!("Device disconnected");
                }
                DeviceEvent::DataPacket(packet) => {
                    // 将设备数据包转换为处理后的数据
                    match convert_data_packet(packet).await {
                        Ok(processed) => {
                            packet_count += 1;
                            let _ = pkt_tx_clone.send(packet_count);
                            let _ = processed_tx_clone.send(processed);
                        }
                        Err(e) => {
                            error!("Failed to convert data packet: {}", e);
                        }
                    }
                }
                DeviceEvent::StatusUpdate(status) => {
                    info!("Device status: connected={}, id={:?}, fw={:?}", 
                          status.connected, status.device_id, status.firmware_version);
                }
                DeviceEvent::LogMessage { level, message } => {
                    match level {
                        0 => tracing::debug!("Device: {}", message),
                        1 => tracing::info!("Device: {}", message),
                        2 => tracing::warn!("Device: {}", message),
                        _ => tracing::error!("Device: {}", message),
                    }
                }
                DeviceEvent::Error(err) => {
                    error!("Device error: {}", err);
                }
                DeviceEvent::FrameReceived(frame) => {
                    info!("Device frame: cmd=0x{:02X}, seq={}, len={}", 
                          frame.command_id, frame.sequence, frame.payload.len());
                }
            }
        }
        warn!("Device event processing loop ended");
    });

    // ======= WebSocket 服务：广播处理后的数据 =======
    let mut ws_server = websocket::WebSocketServer::new(cfg.websocket.clone(), processed_rx_for_ws);
    let ws_clients_rx = ws_server.client_count_rx.clone();
    let ws_handle = tokio::spawn(async move {
        if let Err(e) = ws_server.run().await {
            error!("WebSocket server error: {}", e);
        }
    });

    // ======= Web API（Axum）=======
    let web = web_server::WebServer::new(
        cfg.clone(),
        device_command_tx,
        pkt_rx.clone(),
        ws_clients_rx.clone(),
    );
    let http_handle = tokio::spawn(async move {
        if let Err(e) = web.run().await {
            error!("Web server error: {}", e);
        }
    });

    info!("All services started successfully");
    info!("WebSocket server: ws://{}:{}", cfg.websocket.host, cfg.websocket.port);
    info!("HTTP server: http://{}:{}", cfg.web_server.host, cfg.web_server.port);
    info!("Press Ctrl+C to shutdown");

    // 等待任一任务退出或 Ctrl+C
    tokio::select! {
        _ = device_handle => {
            error!("Device manager terminated");
        }
        _ = event_handle => {
            error!("Event processing task terminated");
        }
        _ = ws_handle => {
            error!("WebSocket server terminated");
        }
        _ = http_handle => {
            error!("Web server terminated");
        }
        _ = tokio::signal::ctrl_c() => {
            info!("Received shutdown signal");
        }
    }

    info!("Shutting down gracefully...");
    tokio::time::sleep(tokio::time::Duration::from_millis(100)).await;
    info!("Shutdown complete");
    Ok(())
}

async fn convert_data_packet(packet: DataPacket) -> Result<ProcessedData> {
    // 示例：把 payload 视为单通道 int16 LE，转换成电压（12bit, 3.3V参考）
    let mut samples = Vec::with_capacity(packet.sensor_data.len() / 2);
    for ch in packet.sensor_data.chunks_exact(2) {
        let raw = u16::from_le_bytes([ch[0], ch[1]]);
        let voltage = (raw as f64 / 4095.0) * 3.3;
        samples.push(voltage);
    }

    // 简单 5 点滑动平均
    let filtered = if samples.len() > 5 {
        let mut out = Vec::with_capacity(samples.len());
        for i in 0..samples.len() {
            if i < 2 || i + 2 >= samples.len() {
                out.push(samples[i]);
            } else {
                let avg = (samples[i-2] + samples[i-1] + samples[i] + samples[i+1] + samples[i+2]) / 5.0;
                out.push(avg);
            }
        }
        out
    } else { samples };

    // 数据质量
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
        sequence: 0,
        channel_count: packet.channel_mask.count_ones() as usize,
        sample_rate: 10_000.0, // 示例值，可由设备配置/INFO带出
        data: filtered,
        metadata: DataMetadata { packet_count: 0, processing_time_us: 0, data_quality: quality },
    })
}