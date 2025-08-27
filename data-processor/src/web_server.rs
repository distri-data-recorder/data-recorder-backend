use crate::config::{Config, StorageConfig};
use crate::file_manager::{FileManager, FileInfo, ProcessedDataFile};
use crate::device_communication::{DeviceCommand, ChannelConfig};
use anyhow::Result;
use axum::{
    extract::{Path, Query, State},
    http::{header, StatusCode},
    response::{IntoResponse, Json, Response},
    routing::{get, post},
    Router,
};
use data_encoding::BASE64;
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::sync::Arc;
use std::time::Instant;
use tokio::sync::{watch, Mutex, mpsc};
use tower::ServiceBuilder;
use tower_http::cors::CorsLayer;
use tracing::{info, warn, error};

#[derive(Debug, Serialize, Deserialize)]
pub struct ApiResponse<T> {
    pub success: bool,
    pub data: Option<T>,
    pub error: Option<String>,
    pub timestamp: i64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct ControlCommand {
    pub command: String,
    pub parameters: Option<serde_json::Value>,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct SystemStatus {
    pub data_collection_active: bool,
    pub device_connected: bool,
    pub connected_clients: usize,
    pub packets_processed: u64,
    pub uptime_seconds: u64,
    pub memory_usage_mb: f64,
    pub connection_type: String,
    pub current_mode: Option<String>,  // 新增：当前模式（continuous/trigger）
    pub trigger_support: bool,         // 新增：触发支持状态
}

#[derive(Clone)]
pub struct AppState {
    cfg: Config,
    device_command_tx: mpsc::UnboundedSender<DeviceCommand>,
    start_at: Instant,
    packets_rx: watch::Receiver<u64>,
    clients_rx: watch::Receiver<usize>,
    collecting: Arc<Mutex<bool>>,
    device_connected: Arc<Mutex<bool>>,
    current_mode: Arc<Mutex<Option<String>>>,  // 新增：跟踪当前模式
    file_manager: Arc<FileManager>,
}

pub struct WebServer {
    state: AppState,
}

impl WebServer {
    pub fn new(
        config: Config,
        device_command_tx: mpsc::UnboundedSender<DeviceCommand>,
        packets_rx: watch::Receiver<u64>,
        clients_rx: watch::Receiver<usize>,
    ) -> Self {
        let fm = FileManager::new(&config.storage.data_dir)
            .expect("failed to init data directory");
        Self {
            state: AppState {
                cfg: config,
                device_command_tx,
                start_at: Instant::now(),
                packets_rx,
                clients_rx,
                collecting: Arc::new(Mutex::new(false)),
                device_connected: Arc::new(Mutex::new(false)),
                current_mode: Arc::new(Mutex::new(None)),
                file_manager: Arc::new(fm),
            },
        }
    }

    pub async fn run(&self) -> Result<()> {
        let app = self.create_router();

        let addr = format!(
            "{}:{}",
            self.state.cfg.web_server.host, self.state.cfg.web_server.port
        );
        info!("Starting HTTP server on {}", addr);

        let listener = tokio::net::TcpListener::bind(&addr).await?;
        axum::serve(listener, app).await?;
        Ok(())
    }

    fn create_router(&self) -> Router {
        Router::new()
            // 控制API
            .route("/api/control/start", post(start_collection))
            .route("/api/control/stop", post(stop_collection))
            .route("/api/control/status", get(get_status))
            .route("/api/control/ping", post(send_ping))
            .route("/api/control/device_info", post(get_device_info))
            .route("/api/control/configure", post(configure_stream))
            .route("/api/control/continuous_mode", post(set_continuous_mode))
            .route("/api/control/trigger_mode", post(set_trigger_mode))
            .route("/api/control/request_trigger_data", post(request_trigger_data))  // 添加这个路由
            // 文件管理API
            .route("/api/files", get(list_files))
            .route("/api/files/:filename", get(download_file))
            .route("/api/files/save", post(save_waveform))
            // 健康检查
            .route("/health", get(health_check))
            // 根路径重定向到API文档
            .route("/", get(api_info))
            .with_state(self.state.clone())
            .layer(ServiceBuilder::new().layer(CorsLayer::permissive()))
    }
}

/// ============ API 处理函数 ============

async fn start_collection(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("API: Start collection requested");
    
    {
        let mut c = st.collecting.lock().await;
        *c = true;
    }
    
    // 发送启动流命令
    if let Err(err) = st.device_command_tx.send(DeviceCommand::StartStream) {
        error!("Failed to send start command: {}", err);
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Data collection started".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn stop_collection(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("API: Stop collection requested");
    
    {
        let mut c = st.collecting.lock().await;
        *c = false;
    }
    
    // 发送停止流命令
    if let Err(err) = st.device_command_tx.send(DeviceCommand::StopStream) {
        error!("Failed to send stop command: {}", err);
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Data collection stopped".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn send_ping(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("API: Ping device requested");
    
    // 发送PING命令
    if let Err(err) = st.device_command_tx.send(DeviceCommand::Ping) {
        error!("Failed to send ping command: {}", err);
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }
    
    Ok(Json(ApiResponse {
        success: true,
        data: Some("Ping command sent to device".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn get_device_info(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("API: Device info requested");
    
    // 发送获取设备信息命令
    if let Err(err) = st.device_command_tx.send(DeviceCommand::GetDeviceInfo) {
        error!("Failed to send device info command: {}", err);
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }
    
    Ok(Json(ApiResponse {
        success: true,
        data: Some("Device info request sent".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn set_continuous_mode(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("API: Set continuous mode requested");
    
    {
        let mut mode = st.current_mode.lock().await;
        *mode = Some("continuous".to_string());
    }
    
    // 发送连续模式命令
    if let Err(err) = st.device_command_tx.send(DeviceCommand::SetModeContinuous) {
        error!("Failed to send continuous mode command: {}", err);
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }
    
    Ok(Json(ApiResponse {
        success: true,
        data: Some("Continuous mode command sent".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn set_trigger_mode(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("API: Set trigger mode requested");
    
    {
        let mut mode = st.current_mode.lock().await;
        *mode = Some("trigger".to_string());
    }
    
    // 发送触发模式命令
    if let Err(err) = st.device_command_tx.send(DeviceCommand::SetModeTrigger) {
        error!("Failed to send trigger mode command: {}", err);
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }
    
    Ok(Json(ApiResponse {
        success: true,
        data: Some("Trigger mode command sent".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn request_trigger_data(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("API: Request trigger data requested");
    
    // 检查是否在触发模式
    {
        let mode = st.current_mode.lock().await;
        if mode.as_deref() != Some("trigger") {
            warn!("Request trigger data called but not in trigger mode");
            return Ok(Json(ApiResponse {
                success: false,
                data: None,
                error: Some("Device not in trigger mode".to_string()),
                timestamp: chrono::Utc::now().timestamp_millis(),
            }));
        }
    }
    
    // 发送请求缓冲数据命令
    if let Err(err) = st.device_command_tx.send(DeviceCommand::RequestBufferedData) {
        error!("Failed to send request buffered data command: {}", err);
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }
    
    Ok(Json(ApiResponse {
        success: true,
        data: Some("Buffered data request sent".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

#[derive(Debug, Deserialize)]
struct ConfigureRequest {
    channels: Vec<ChannelConfigRequest>,
}

#[derive(Debug, Deserialize)]
struct ChannelConfigRequest {
    channel_id: u8,
    sample_rate: u32,
    format: u8,
}

async fn configure_stream(
    State(st): State<AppState>,
    Json(req): Json<ConfigureRequest>,
) -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("API: Configure stream requested with {} channels", req.channels.len());
    
    let channels: Vec<ChannelConfig> = req.channels.into_iter()
        .map(|c| ChannelConfig {
            channel_id: c.channel_id,
            sample_rate: c.sample_rate,
            format: c.format,
        })
        .collect();
    
    // 发送配置流命令
    if let Err(err) = st.device_command_tx.send(DeviceCommand::ConfigureStream { channels }) {
        error!("Failed to send configure command: {}", err);
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }
    
    Ok(Json(ApiResponse {
        success: true,
        data: Some("Stream configuration sent".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn get_status(State(st): State<AppState>) -> Result<Json<ApiResponse<SystemStatus>>, StatusCode> {
    // 汇总当前状态
    let packets = *st.packets_rx.borrow();
    let clients = *st.clients_rx.borrow();
    let collecting = *st.collecting.lock().await;
    let device_connected = *st.device_connected.lock().await;
    let current_mode = st.current_mode.lock().await.clone();

    let status = SystemStatus {
        data_collection_active: collecting,
        device_connected,
        connected_clients: clients,
        packets_processed: packets,
        uptime_seconds: st.start_at.elapsed().as_secs(),
        memory_usage_mb: get_memory_usage_mb(),
        connection_type: st.cfg.device.connection_type.clone(),
        current_mode,
        trigger_support: true,  // 标识触发支持已启用
    };

    Ok(Json(ApiResponse {
        success: true,
        data: Some(status),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

/// GET /api/files?dir=相对目录
#[derive(Debug, Deserialize)]
struct ListQuery {
    dir: Option<String>,
}

async fn list_files(
    State(st): State<AppState>,
    Query(q): Query<ListQuery>,
) -> Result<Json<ApiResponse<Vec<FileInfo>>>, StatusCode> {
    match st.file_manager.list_files_in(q.dir.as_deref()) {
        Ok(files) => {
            info!("Listed {} files in dir: {:?}", files.len(), q.dir);
            Ok(Json(ApiResponse {
                success: true,
                data: Some(files),
                error: None,
                timestamp: chrono::Utc::now().timestamp_millis(),
            }))
        }
        Err(e) => {
            warn!("list_files failed: {}", e);
            Err(StatusCode::INTERNAL_SERVER_ERROR)
        }
    }
}

/// GET /api/files/:filename   （支持子目录：例如 runs/2025-08-26/wave.bin）
async fn download_file(
    State(st): State<AppState>,
    Path(filename): Path<String>,
) -> Result<Response, StatusCode> {
    match st.file_manager.read_file(&filename) {
        Ok(bytes) => {
            let cd = format!(
                "attachment; filename=\"{}\"", 
                filename.split(|c| c == '/' || c == '\\').last().unwrap_or(&filename)
            );
            let headers = [
                (header::CONTENT_TYPE, "application/octet-stream"),
                (header::CONTENT_DISPOSITION, cd.as_str()),
            ];
            info!("Downloaded file: {} ({} bytes)", filename, bytes.len());
            Ok((headers, bytes).into_response())
        }
        Err(e) => {
            warn!("download_file failed: {} ({})", filename, e);
            Err(StatusCode::NOT_FOUND)
        }
    }
}

#[derive(Debug, Serialize, Deserialize)]
struct SaveRequest {
    /// 相对 base 的子目录，例如 "runs/2025-08-26"（可选）
    dir: Option<String>,
    /// 文件名（可选；未提供则自动命名）
    filename: Option<String>,
    /// base64 编码的文件内容（必填）
    base64: String,
}

/// POST /api/files/save
/// body: { "dir":"runs/2025-08-26", "filename":"my.bin", "base64":"..." }
/// - 若未提供 filename，则自动命名：{prefix}_{YYYYMMDD_HHMMSS}{ext}
async fn save_waveform(
    State(st): State<AppState>,
    Json(req): Json<SaveRequest>,
) -> Result<Json<ApiResponse<String>>, StatusCode> {
    // base64 -> bytes
    let bytes = match BASE64.decode(req.base64.as_bytes()) {
        Ok(b) => b,
        Err(e) => {
            warn!("save_waveform: base64 decode error: {}", e);
            return Err(StatusCode::BAD_REQUEST);
        }
    };

    // 文件名：若未提供或为空，则根据配置自动生成
    let filename = req
        .filename
        .and_then(|s| {
            let s = s.trim().to_string();
            if s.is_empty() { None } else { Some(s) }
        })
        .unwrap_or_else(|| make_auto_filename(&st.cfg.storage));

    let data = ProcessedDataFile {
        filename: filename.clone(),
        bytes,
    };

    match st.file_manager.save_at(req.dir.as_deref(), &data) {
        Ok(saved_rel_path) => {
            // 限制 base 根目录下的总文件数（不递归）
            let _ = st.file_manager.cleanup_old_files(st.cfg.storage.max_files);

            info!("Saved file: {} ({} bytes)", saved_rel_path, data.bytes.len());
            Ok(Json(ApiResponse {
                success: true,
                data: Some(saved_rel_path),
                error: None,
                timestamp: chrono::Utc::now().timestamp_millis(),
            }))
        }
        Err(e) => {
            error!("save_waveform failed: {}", e);
            Err(StatusCode::INTERNAL_SERVER_ERROR)
        }
    }
}

async fn health_check() -> Json<ApiResponse<serde_json::Value>> {
    Json(ApiResponse {
        success: true,
        data: Some(json!({
            "status": "healthy",
            "service": "data-processor",
            "version": "2.0",
            "trigger_support": true,
            "timestamp": chrono::Utc::now().to_rfc3339()
        })),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    })
}

async fn api_info() -> Json<serde_json::Value> {
    Json(json!({
        "name": "Integrated Data Processor API",
        "version": "2.0",
        "description": "High-performance data acquisition and processing system with trigger support",
        "features": {
            "continuous_mode": true,
            "trigger_mode": true,
            "websocket_streaming": true,
            "file_management": true,
            "real_time_processing": true
        },
        "endpoints": {
            "health": "/health",
            "status": "/api/control/status",
            "start": "/api/control/start",
            "stop": "/api/control/stop",
            "ping": "/api/control/ping",
            "device_info": "/api/control/device_info",
            "modes": {
                "continuous": "/api/control/continuous_mode",
                "trigger": "/api/control/trigger_mode"
            },
            "trigger": {
                "request_data": "/api/control/request_trigger_data"
            },
            "configuration": "/api/control/configure",
            "files": {
                "list": "/api/files?dir=<optional>",
                "download": "/api/files/{filename}",
                "save": "/api/files/save"
            },
            "websocket": "ws://<host>:<port>"
        },
        "documentation": "https://github.com/your-repo/data-processor"
    }))
}

// 辅助函数
fn make_auto_filename(sto: &StorageConfig) -> String {
    let ts = chrono::Local::now().format("%Y%m%d_%H%M%S").to_string();
    format!("{}_{}{}", sto.default_prefix, ts, sto.default_ext)
}

fn get_memory_usage_mb() -> f64 {
    // 简单的内存使用统计，可以用sysinfo库获取更精确的数据
    #[cfg(target_os = "windows")]
    {
        // Windows平台可以通过GetProcessMemoryInfo获取
        0.0
    }
    #[cfg(not(target_os = "windows"))]
    {
        // Unix平台可以读取/proc/self/status
        0.0
    }
}