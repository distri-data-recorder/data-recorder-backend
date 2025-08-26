mod ipc;
mod data_processing;
mod web_server;
mod websocket;
mod file_manager;
mod config;

use anyhow::Result;
use tokio::sync::watch;
use tracing::{error, info, warn};

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt::init();
    info!("Starting Data Processor Service");

    // 加载配置
    let cfg = config::Config::load()?;
    info!("Configuration loaded: {:?}", cfg);

    // 连接共享内存
    let shared_mem = match ipc::SharedMemoryReader::new(&cfg.shared_memory_name) {
        Ok(mem) => {
            info!("Connected to shared memory");
            mem
        }
        Err(e) => {
            warn!("Failed to connect to shared memory: {}. Exiting.", e);
            return Err(e);
        }
    };

    // 启动命名管道 IPC 客户端（与 data-reader 保持一致管道名）
    let ipc_client = ipc::IpcClient::start(r"\\.\pipe\data_reader_ipc")?;

    // 订阅 data-reader 下行消息，打印到日志（避免 subscribe 未使用的告警）
    let mut ipc_rx = ipc_client.subscribe();
    tokio::spawn(async move {
        while let Ok(v) = ipc_rx.recv().await {
            if let Some(t) = v.get("type").and_then(|x| x.as_str()) {
                match t {
                    "READER_STATUS_UPDATE" => tracing::info!("IPC status: {}", v),
                    "DEVICE_FRAME_RECEIVED" => tracing::debug!("IPC frame: {}", v),
                    "DEVICE_LOG_RECEIVED" => tracing::info!("IPC log: {}", v),
                    _ => tracing::debug!("IPC msg: {}", v),
                }
            } else {
                tracing::debug!("IPC msg: {}", v);
            }
        }
    });

    // ======= 数据处理器 =======
    let mut processor = data_processing::DataProcessor::new(shared_mem);

    // 在把 processor move 进 spawn 之前，先拿到两个 Receiver
    let processed_rx_for_stat = processor.get_data_receiver(); // 统计用
    let processed_rx_for_ws   = processor.get_data_receiver(); // WebSocket 广播用

    // 用 watch 跟踪“累计处理包数”供 Web API 查询
    let (pkt_tx, pkt_rx) = watch::channel(0u64);

    // ======= 数据处理任务（把 processor move 进去）=======
    let processing_handle = tokio::spawn(async move {
        // 订阅数据流做一个简单 packet 计数
        let mut rx = processed_rx_for_stat;
        let pkt_tx_for_stat = pkt_tx.clone();
        tokio::spawn(async move {
            let mut cnt: u64 = 0;
            while let Ok(_data) = rx.recv().await {
                cnt += 1;
                let _ = pkt_tx_for_stat.send(cnt);
            }
        });

        if let Err(e) = processor.run().await {
            error!("Data processing error: {}", e);
        }
    });

    // ======= WebSocket 服务：广播处理后的数据 =======
    let mut ws_server =
        websocket::WebSocketServer::new(cfg.websocket.clone(), processed_rx_for_ws);
    let ws_clients_rx = ws_server.client_count_rx.clone();
    let ws_handle = tokio::spawn(async move {
        if let Err(e) = ws_server.run().await {
            error!("WebSocket server error: {}", e);
        }
    });

    // ======= Web API（Axum）=======
    let web = web_server::WebServer::new(
        cfg.clone(),
        ipc_client.clone(),
        pkt_rx.clone(),
        ws_clients_rx.clone(),
    );
    let http_handle = tokio::spawn(async move {
        if let Err(e) = web.run().await {
            error!("Web server error: {}", e);
        }
    });

    info!("All services started");

    // 等待任一任务退出或 Ctrl+C
    tokio::select! {
        _ = processing_handle => {
            error!("Data processing task terminated");
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

    info!("Shutting down");
    Ok(())
}
