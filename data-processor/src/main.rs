mod device_communication;
mod data_processing;
mod web_server;
mod websocket;
mod file_manager;
mod config;

use anyhow::Result;
use std::sync::Arc;
use tokio::sync::{watch, Mutex};
use tracing::{error, info, warn};
use crate::data_processing::DataProcessor;
use device_communication::{DeviceManager, DeviceConfig, ConnectionType, DeviceEvent};

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt::init();
    info!("Starting Integrated Data Processor v2.0 with Enhanced Trigger Support");

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

    // 统计数据包数量
    let (pkt_tx, pkt_rx) = watch::channel(0u64);
    
    // 用于追踪设备连接状态
    let (device_status_tx, device_status_rx) = watch::channel(false);
    
    // 用于广播处理后的数据到WebSocket
    let (processed_tx, processed_rx_for_ws) = tokio::sync::broadcast::channel(1000);
    
    // 用于WebSocket广播触发事件
    let (trigger_event_tx, trigger_event_rx) = tokio::sync::broadcast::channel(100);
    
    // 用于WebSocket广播触发批次完成事件
    let (trigger_burst_complete_tx, trigger_burst_complete_rx) = tokio::sync::broadcast::channel(100);

    // 创建共享的数据处理器
    let data_processor = Arc::new(Mutex::new(DataProcessor::new()));

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
    let trigger_event_tx_clone = trigger_event_tx.clone();
    let trigger_burst_complete_tx_clone = trigger_burst_complete_tx.clone();
    let pkt_tx_clone = pkt_tx.clone();
    let data_processor_clone = data_processor.clone();
    
    let event_handle = tokio::spawn(async move {
        let mut packet_count = 0u64;
        let mut _current_burst_id: Option<String> = None;
        
        while let Some(event) = device_events.recv().await {
            match event {
                DeviceEvent::Connected(conn_type) => {
                    info!("Device connected: {}", conn_type);
                    let _ = device_status_tx.send(true); // 更新连接状态
                    // 重置数据处理器状态
                    let mut processor = data_processor_clone.lock().await;
                    processor.reset_trigger_state();
                    _current_burst_id = None;
                }
                DeviceEvent::Disconnected => {
                    warn!("Device disconnected");
                    let _ = device_status_tx.send(false); // 更新断开状态
                    _current_burst_id = None;
                }
                DeviceEvent::TriggerEvent(trigger_event) => {
                    info!("Trigger event received: timestamp={}, channel={}, pre={}, post={}", 
                          trigger_event.timestamp, trigger_event.channel, 
                          trigger_event.pre_samples, trigger_event.post_samples);
                    
                    // 开始新的触发批次
                    let burst_id = {
                        let mut processor = data_processor_clone.lock().await;
                        processor.start_trigger_burst(&trigger_event)
                    };
                    _current_burst_id = Some(burst_id);
                    
                    // 广播触发事件到WebSocket客户端
                    let _ = trigger_event_tx_clone.send(trigger_event);
                }
                DeviceEvent::DataPacket(packet) => {
                    // 收到数据包表示设备连接正常
                    let _ = device_status_tx.send(true); // 数据活跃时更新状态
                    
                    // 处理数据包
                    let processed_result = {
                        let mut processor = data_processor_clone.lock().await;
                        processor.process_packet(&packet)
                    };

                    match processed_result {
                        Ok(processed) => {
                            packet_count += 1;
                            let _ = pkt_tx_clone.send(packet_count);
                            
                            // 日志记录，区分连续和触发数据
                            let data_len = processed.data.len();
                            let data_source = processed.data_type.source.clone();
                            let trigger_info = processed.data_type.trigger_info.clone();
                            
                            // 广播处理后的数据
                            let _ = processed_tx_clone.send(processed);
                            
                            match data_source {
                                crate::data_processing::DataSource::Continuous => {
                                    if packet_count % 100 == 0 { // 每100包记录一次，避免日志过多
                                        info!("Processed continuous data packet #{}, {} samples", 
                                              packet_count, data_len);
                                    }
                                }
                                crate::data_processing::DataSource::Trigger => {
                                    if let Some(ref trigger_info) = trigger_info {
                                        info!("Processed trigger data packet #{}, sequence in burst: {}, {} samples", 
                                              packet_count, 
                                              trigger_info.sequence_in_burst.unwrap_or(0), 
                                              data_len);
                                    }
                                }
                            }
                        }
                        Err(e) => {
                            error!("Failed to process data packet: {}", e);
                        }
                    }
                }
                DeviceEvent::BufferTransferComplete => {
                    info!("Trigger data transfer completed");
                    
                    // 完成当前触发批次
                    let completed_burst = {
                        let mut processor = data_processor_clone.lock().await;
                        processor.complete_trigger_burst()
                    };
                    
                    if let Some(burst) = completed_burst {
                        let stats = {
                            let processor = data_processor_clone.lock().await;
                            processor.get_stats()
                        };
                        
                        info!("Trigger burst completed: id={}, packets={}, samples={}", 
                              burst.burst_id, burst.data_packets.len(), burst.total_samples);
                        
                        // 广播触发批次完成事件到WebSocket客户端
                        let _ = trigger_burst_complete_tx_clone.send(burst);
                        
                        // 处理统计信息
                        info!("Processing stats: total_packets={}, trigger_burst_seq={}", 
                              stats.total_packets_processed, stats.current_trigger_burst_sequence);
                    }
                    
                    _current_burst_id = None;
                }
                DeviceEvent::StatusUpdate(status) => {
                    // 收到设备状态更新也表示连接正常
                    let _ = device_status_tx.send(true); // 响应活跃时更新状态
                    
                    info!("Device status: connected={}, id={:?}, fw={:?}, mode={:?}", 
                          status.connected, status.device_id, status.firmware_version, status.mode);
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
                    // 只在调试模式下记录帧信息，避免日志过多
                    tracing::debug!("Device frame: cmd=0x{:02X}, seq={}, len={}", 
                                   frame.command_id, frame.sequence, frame.payload.len());
                }
            }
        }
        warn!("Device event processing loop ended");
    });

    // ======= WebSocket 服务：广播处理后的数据、触发事件和批次完成事件 =======
    let mut ws_server = websocket::WebSocketServer::new(
        cfg.websocket.clone(), 
        processed_rx_for_ws,
        trigger_event_rx,
        trigger_burst_complete_rx
    );
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
        data_processor.clone(),
        device_status_rx,
    );
    let http_handle = tokio::spawn(async move {
        if let Err(e) = web.run().await {
            error!("Web server error: {}", e);
        }
    });

    info!("All services started successfully");
    info!("WebSocket server: ws://{}:{}", cfg.websocket.host, cfg.websocket.port);
    info!("HTTP server: http://{}:{}", cfg.web_server.host, cfg.web_server.port);
    info!("Enhanced trigger mode support: ENABLED");
    info!("  - Real-time trigger data preview");
    info!("  - Custom file saving with user-defined paths");
    info!("  - Multiple export formats (JSON, CSV, Binary)");
    info!("  - Quality assessment and statistics");
    info!("  - Trigger burst management and caching");
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