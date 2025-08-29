use crate::config::{Config, StorageConfig};
use crate::file_manager::{FileManager, FileInfo, ProcessedDataFile};
use crate::device_communication::{DeviceCommand, ChannelConfig};
use crate::data_processing::{DataProcessor, TriggerSummary, TriggerBurst};
use anyhow::Result;
use axum::{
    extract::{Path, Query, State},
    http::{header, StatusCode},
    response::{IntoResponse, Json, Response},
    routing::{get, post, delete},
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
    pub current_mode: Option<String>,
    pub trigger_support: bool,
    pub trigger_status: Option<TriggerStatus>,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct TriggerStatus {
    pub cached_bursts: usize,
    pub current_burst_active: bool,
    pub last_trigger_timestamp: Option<u32>,
    pub total_triggers_received: u64,
}

#[derive(Clone)]
pub struct AppState {
    cfg: Config,
    device_command_tx: mpsc::UnboundedSender<DeviceCommand>,
    start_at: Instant,
    packets_rx: watch::Receiver<u64>,
    clients_rx: watch::Receiver<usize>,
    collecting: Arc<Mutex<bool>>,
    device_status_rx: watch::Receiver<bool>,
    current_mode: Arc<Mutex<Option<String>>>,
    file_manager: Arc<FileManager>,
    data_processor: Arc<Mutex<DataProcessor>>,
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
        data_processor: Arc<Mutex<DataProcessor>>,
        device_status_rx: watch::Receiver<bool>,
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
                device_status_rx,
                current_mode: Arc::new(Mutex::new(None)),
                file_manager: Arc::new(fm),
                data_processor,
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
            .route("/api/control/request_trigger_data", post(request_trigger_data))
            // 触发数据管理API
            .route("/api/trigger/list", get(list_trigger_bursts))
            .route("/api/trigger/preview/:burst_id", get(preview_trigger_burst))
            .route("/api/trigger/save/:burst_id", post(save_trigger_burst))
            .route("/api/trigger/delete/:burst_id", delete(delete_trigger_burst))
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

// ============ API 处理函数 ============

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

// ============ 触发数据管理 ============

/// 获取触发批次列表
async fn list_trigger_bursts(
    State(st): State<AppState>
) -> Result<Json<ApiResponse<Vec<TriggerSummary>>>, StatusCode> {
    let processor = st.data_processor.lock().await;
    let summaries = processor.get_trigger_summaries();
    
    info!("Listed {} trigger bursts", summaries.len());
    
    Ok(Json(ApiResponse {
        success: true,
        data: Some(summaries),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

/// 预览触发批次详细信息
async fn preview_trigger_burst(
    State(st): State<AppState>,
    Path(burst_id): Path<String>
) -> Result<Json<ApiResponse<TriggerBurst>>, StatusCode> {
    let processor = st.data_processor.lock().await;
    
    match processor.get_trigger_burst(&burst_id) {
        Some(burst) => {
            info!("Previewed trigger burst: {}", burst_id);
            Ok(Json(ApiResponse {
                success: true,
                data: Some(burst.clone()),
                error: None,
                timestamp: chrono::Utc::now().timestamp_millis(),
            }))
        }
        None => {
            warn!("Trigger burst not found: {}", burst_id);
            Err(StatusCode::NOT_FOUND)
        }
    }
}

#[derive(Debug, Serialize, Deserialize)]
struct SaveTriggerRequest {
    /// 保存的子目录路径（相对于data_dir）
    pub dir: Option<String>,
    /// 自定义文件名（不含扩展名）
    pub filename: Option<String>,
    /// 导出格式：json, csv, binary
    pub format: String,
    /// 文件描述或备注
    pub description: Option<String>,
}

#[derive(Debug, Serialize, Deserialize)]
struct SaveTriggerResponse {
    pub saved_path: String,
    pub format: String,
    pub size_bytes: usize,
    pub burst_info: TriggerSummary,
}

/// 保存触发批次数据
async fn save_trigger_burst(
    State(st): State<AppState>,
    Path(burst_id): Path<String>,
    Json(req): Json<SaveTriggerRequest>
) -> Result<Json<ApiResponse<SaveTriggerResponse>>, StatusCode> {
    // 验证格式
    let valid_formats = ["json", "csv", "binary"];
    if !valid_formats.contains(&req.format.as_str()) {
        return Ok(Json(ApiResponse {
            success: false,
            data: None,
            error: Some(format!("Invalid format. Supported: {:?}", valid_formats)),
            timestamp: chrono::Utc::now().timestamp_millis(),
        }));
    }

    // 获取数据
    let (burst_data, burst_summary) = {
        let processor = st.data_processor.lock().await;
        
        let burst = match processor.get_trigger_burst(&burst_id) {
            Some(b) => b,
            None => {
                return Ok(Json(ApiResponse {
                    success: false,
                    data: None,
                    error: Some("Trigger burst not found".to_string()),
                    timestamp: chrono::Utc::now().timestamp_millis(),
                }));
            }
        };

        if !burst.is_complete {
            return Ok(Json(ApiResponse {
                success: false,
                data: None,
                error: Some("Trigger burst is not complete yet".to_string()),
                timestamp: chrono::Utc::now().timestamp_millis(),
            }));
        }

        let data = match processor.export_trigger_burst(&burst_id, &req.format) {
            Ok(data) => data,
            Err(e) => {
                error!("Failed to export trigger burst {}: {}", burst_id, e);
                return Err(StatusCode::INTERNAL_SERVER_ERROR);
            }
        };

        let summary = TriggerSummary {
            burst_id: burst.burst_id.clone(),
            trigger_timestamp: burst.trigger_timestamp,
            trigger_channel: burst.trigger_channel,
            total_samples: burst.total_samples,
            duration_ms: processor.calculate_duration_ms(burst),
            created_at: burst.created_at,
            quality: match burst.quality_summary.overall_quality {
                crate::data_processing::DataQuality::Good => "Good".to_string(),
                crate::data_processing::DataQuality::Warning(_) => "Warning".to_string(),
                crate::data_processing::DataQuality::Error(_) => "Error".to_string(),
            },
            can_save: true,
        };

        (data, summary)
    };

    // 生成文件名
    let extension = match req.format.as_str() {
        "json" => ".json",
        "csv" => ".csv",
        "binary" => ".bin",
        _ => ".dat",
    };

    let filename = req.filename
        .as_deref()
        .filter(|s| !s.trim().is_empty())
        .map(|s| format!("{}{}", s.trim(), extension))
        .unwrap_or_else(|| {
            let timestamp = chrono::DateTime::from_timestamp_millis(burst_summary.created_at)
                .unwrap_or_else(chrono::Utc::now)
                .format("%Y%m%d_%H%M%S");
            format!("trigger_{}_{}{}", 
                   burst_summary.trigger_timestamp, 
                   timestamp, 
                   extension)
        });

    // 创建文件对象
    let mut file_data = ProcessedDataFile {
        filename: filename.clone(),
        bytes: burst_data,
    };

    // 如果是JSON格式，添加元数据
    if req.format == "json" {
        let metadata = serde_json::json!({
            "saved_at": chrono::Utc::now().to_rfc3339(),
            "description": req.description,
            "format": req.format,
            "burst_summary": burst_summary
        });
        
        // 将元数据插入到JSON的开头
        if let Ok(mut json_data) = serde_json::from_slice::<serde_json::Value>(&file_data.bytes) {
            json_data["metadata"] = metadata;
            file_data.bytes = serde_json::to_string_pretty(&json_data)
                .unwrap_or_else(|_| String::from_utf8_lossy(&file_data.bytes).to_string())
                .into_bytes();
        }
    }

    // 保存文件
    match st.file_manager.save_at(req.dir.as_deref(), &file_data) {
        Ok(saved_rel_path) => {
            // 限制文件数量
            let _ = st.file_manager.cleanup_old_files(st.cfg.storage.max_files);

            info!("Saved trigger burst {} to {}", burst_id, saved_rel_path);

            let response = SaveTriggerResponse {
                saved_path: saved_rel_path,
                format: req.format,
                size_bytes: file_data.bytes.len(),
                burst_info: burst_summary,
            };

            Ok(Json(ApiResponse {
                success: true,
                data: Some(response),
                error: None,
                timestamp: chrono::Utc::now().timestamp_millis(),
            }))
        }
        Err(e) => {
            error!("Failed to save trigger burst {}: {}", burst_id, e);
            Err(StatusCode::INTERNAL_SERVER_ERROR)
        }
    }
}

/// 删除缓存的触发批次
async fn delete_trigger_burst(
    State(st): State<AppState>,
    Path(burst_id): Path<String>
) -> Result<Json<ApiResponse<String>>, StatusCode> {
    let mut processor = st.data_processor.lock().await;
    
    if processor.remove_trigger_burst(&burst_id) {
        info!("Deleted trigger burst: {}", burst_id);
        
        Ok(Json(ApiResponse {
            success: true,
            data: Some("Trigger burst deleted".to_string()),
            error: None,
            timestamp: chrono::Utc::now().timestamp_millis(),
        }))
    } else {
        warn!("Attempted to delete non-existent trigger burst: {}", burst_id);
        Err(StatusCode::NOT_FOUND)
    }
}

async fn get_status(State(st): State<AppState>) -> Result<Json<ApiResponse<SystemStatus>>, StatusCode> {
    // 汇总当前状态
    let packets = *st.packets_rx.borrow();
    let clients = *st.clients_rx.borrow();
    let collecting = *st.collecting.lock().await;
    let device_connected = *st.device_status_rx.borrow();
    let current_mode = st.current_mode.lock().await.clone();

    // 获取触发状态
    let trigger_status = {
        let processor = st.data_processor.lock().await;
        let stats = processor.get_stats();
        Some(TriggerStatus {
            cached_bursts: stats.cached_bursts_count,
            current_burst_active: stats.current_burst_active,
            last_trigger_timestamp: stats.current_trigger_timestamp,
            total_triggers_received: stats.total_packets_processed,
        })
    };

    let status = SystemStatus {
        data_collection_active: collecting,
        device_connected,
        connected_clients: clients,
        packets_processed: packets,
        uptime_seconds: st.start_at.elapsed().as_secs(),
        memory_usage_mb: get_memory_usage_mb(),
        connection_type: st.cfg.device.connection_type.clone(),
        current_mode,
        trigger_support: true,
        trigger_status,
    };

    Ok(Json(ApiResponse {
        success: true,
        data: Some(status),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

// ============ 文件管理 ============

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
        "description": "High-performance data acquisition and processing system with enhanced trigger support",
        "features": {
            "continuous_mode": true,
            "trigger_mode": true,
            "websocket_streaming": true,
            "file_management": true,
            "real_time_processing": true,
            "trigger_data_management": true,
            "custom_file_saving": true
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
                "request_data": "/api/control/request_trigger_data",
                "list_bursts": "/api/trigger/list",
                "preview_burst": "/api/trigger/preview/{burst_id}",
                "save_burst": "/api/trigger/save/{burst_id}",
                "delete_burst": "/api/trigger/delete/{burst_id}"
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