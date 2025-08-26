use crate::config::{Config, StorageConfig};
use crate::file_manager::{FileManager, FileInfo, ProcessedDataFile};
use crate::ipc::IpcClient;
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
use tokio::sync::{watch, Mutex};
use tower::ServiceBuilder;
use tower_http::cors::CorsLayer;
use tracing::info;

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
    pub connected_clients: usize,
    pub packets_processed: u64,
    pub uptime_seconds: u64,
    pub memory_usage_mb: f64,
}

#[derive(Clone)]
pub struct AppState {
    cfg: Config,
    ipc: Arc<IpcClient>,
    start_at: Instant,
    packets_rx: watch::Receiver<u64>,
    clients_rx: watch::Receiver<usize>,
    collecting: Arc<Mutex<bool>>,
    file_manager: Arc<FileManager>,
}

pub struct WebServer {
    state: AppState,
}

impl WebServer {
    pub fn new(
        config: Config,
        ipc: Arc<IpcClient>,
        packets_rx: watch::Receiver<u64>,
        clients_rx: watch::Receiver<usize>,
    ) -> Self {
        let fm = FileManager::new(&config.storage.data_dir)
            .expect("failed to init data directory");
        Self {
            state: AppState {
                cfg: config,
                ipc,
                start_at: Instant::now(),
                packets_rx,
                clients_rx,
                collecting: Arc::new(Mutex::new(false)),
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
            .route("/api/control/request_status", post(request_reader_status))
            // 文件管理API（接入 FileManager）
            .route("/api/files", get(list_files))
            .route("/api/files/:filename", get(download_file))
            .route("/api/files/save", post(save_waveform))
            // 健康检查
            .route("/health", get(health_check))
            .with_state(self.state.clone())
            .layer(ServiceBuilder::new().layer(CorsLayer::permissive()))
    }
}

/// ============ API 处理函数 ============

async fn start_collection(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    tracing::info!("Received start collection command");
    {
        let mut c = st.collecting.lock().await;
        *c = true;
    }
    // 通过 IPC 转发命令：开始流=0x12
    let msg = json!({
        "id":"web_start",
        "timestamp": chrono::Utc::now().to_rfc3339(),
        "type":"FORWARD_TO_DEVICE",
        "payload": { "command_id":"0x12", "data":"" }
    });
    if let Err(err) = st.ipc.send_json(&msg) {
        tracing::warn!("IPC send failed: {}", err);
    }

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Data collection started".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn stop_collection(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    tracing::info!("Received stop collection command");
    {
        let mut c = st.collecting.lock().await;
        *c = false;
    }
    // 停止流=0x13
    let msg = json!({
        "id":"web_stop",
        "timestamp": chrono::Utc::now().to_rfc3339(),
        "type":"FORWARD_TO_DEVICE",
        "payload": { "command_id":"0x13", "data":"" }
    });
    if let Err(err) = st.ipc.send_json(&msg) {
        tracing::warn!("IPC send failed: {}", err);
    }

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Data collection stopped".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn request_reader_status(State(st): State<AppState>) -> Result<Json<ApiResponse<String>>, StatusCode> {
    // 主动向 data-reader 请求状态
    let msg = json!({
        "id":"web_status_req",
        "timestamp": chrono::Utc::now().to_rfc3339(),
        "type":"REQUEST_READER_STATUS",
        "payload": {}
    });
    if let Err(err) = st.ipc.send_json(&msg) {
        tracing::warn!("IPC send failed: {}", err);
        return Err(StatusCode::BAD_GATEWAY);
    }
    Ok(Json(ApiResponse {
        success: true,
        data: Some("Status requested".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn get_status(State(st): State<AppState>) -> Result<Json<ApiResponse<SystemStatus>>, StatusCode> {
    // 汇总当前状态
    let packets = *st.packets_rx.borrow();
    let clients = *st.clients_rx.borrow();
    let collecting = *st.collecting.lock().await;

    let status = SystemStatus {
        data_collection_active: collecting,
        connected_clients: clients,
        packets_processed: packets,
        uptime_seconds: st.start_at.elapsed().as_secs(),
        memory_usage_mb: 0.0, // 如需可加 sysinfo 读取
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
    match st
        .file_manager
        .list_files_in(q.dir.as_deref())
    {
        Ok(files) => Ok(Json(ApiResponse {
            success: true,
            data: Some(files),
            error: None,
            timestamp: chrono::Utc::now().timestamp_millis(),
        })),
        Err(e) => Err({
            tracing::warn!("list_files failed: {}", e);
            StatusCode::INTERNAL_SERVER_ERROR
        }),
    }
}

/// GET /api/files/:filename   （支持子目录：例如 runs/2025-08-26/wave.bin）
async fn download_file(
    State(st): State<AppState>,
    Path(filename): Path<String>,
) -> Result<Response, StatusCode> {
    match st.file_manager.read_file(&filename) {
        Ok(bytes) => {
            let cd = format!("attachment; filename=\"{}\"", filename.split(|c| c == '/' || c == '\\').last().unwrap_or(&filename));
            let headers = [
                (header::CONTENT_TYPE, "application/octet-stream"),
                (header::CONTENT_DISPOSITION, cd.as_str()),
            ];
            Ok((headers, bytes).into_response())
        }
        Err(e) => {
            tracing::warn!("download_file failed: {} ({})", filename, e);
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
            tracing::warn!("save_waveform: base64 decode error: {}", e);
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
            // 限制 base 根目录下的总文件数（不递归）；如需全局递归可改 file_manager
            let _ = st.file_manager.cleanup_old_files(st.cfg.storage.max_files);

            Ok(Json(ApiResponse {
                success: true,
                data: Some(saved_rel_path),
                error: None,
                timestamp: chrono::Utc::now().timestamp_millis(),
            }))
        }
        Err(e) => {
            tracing::warn!("save_waveform failed: {}", e);
            Err(StatusCode::INTERNAL_SERVER_ERROR)
        }
    }
}

fn make_auto_filename(sto: &StorageConfig) -> String {
    let ts = chrono::Local::now().format("%Y%m%d_%H%M%S").to_string();
    // prefix_YYYYMMDD_HHMMSS.ext
    format!("{}_{}{}", sto.default_prefix, ts, sto.default_ext)
}

async fn health_check() -> Json<ApiResponse<String>> {
    Json(ApiResponse {
        success: true,
        data: Some("OK".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    })
}
