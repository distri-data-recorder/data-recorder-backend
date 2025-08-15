use crate::config::Config;
use anyhow::Result;
use axum::{
    extract::Path,
    http::StatusCode,
    response::{Json, Response},
    routing::{get, post},
    Router,
};
use serde::{Deserialize, Serialize};
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

#[derive(Debug, Serialize, Deserialize)]
pub struct SystemStatus {
    pub data_collection_active: bool,
    pub connected_clients: usize,
    pub packets_processed: u64,
    pub uptime_seconds: u64,
    pub memory_usage_mb: f64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct FileInfo {
    pub filename: String,
    pub size_bytes: u64,
    pub created_at: i64,
    pub file_type: String,
}

pub struct WebServer {
    config: Config,
}

impl WebServer {
    pub fn new(config: Config) -> Self {
        Self { config }
    }

    pub async fn run(&self) -> Result<()> {
        let app = self.create_router();

        let addr = format!("{}:{}", self.config.web_server.host, self.config.web_server.port);
        info!("Starting HTTPS server on {}", addr);

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

            // 文件管理API
            .route("/api/files", get(list_files))
            .route("/api/files/:filename", get(download_file))
            .route("/api/files/save", post(save_waveform))

            // 健康检查
            .route("/health", get(health_check))

            // CORS支持
            .layer(
                ServiceBuilder::new()
                    .layer(CorsLayer::permissive())
            )
    }
}

// API处理函数

async fn start_collection() -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("Received start collection command");

    // TODO: 通过消息队列发送开始采集命令给数据采集进程

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Data collection started".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn stop_collection() -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("Received stop collection command");

    // TODO: 通过消息队列发送停止采集命令给数据采集进程

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Data collection stopped".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn get_status() -> Result<Json<ApiResponse<SystemStatus>>, StatusCode> {
    // TODO: 获取实际的系统状态
    let status = SystemStatus {
        data_collection_active: false, // 从实际状态获取
        connected_clients: 0,          // 从WebSocket服务器获取
        packets_processed: 0,          // 从数据处理器获取
        uptime_seconds: 0,             // 计算实际运行时间
        memory_usage_mb: 0.0,          // 获取内存使用情况
    };

    Ok(Json(ApiResponse {
        success: true,
        data: Some(status),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn list_files() -> Result<Json<ApiResponse<Vec<FileInfo>>>, StatusCode> {
    // TODO: 扫描文件目录，返回可用的波形文件列表
    let files = vec![
        FileInfo {
            filename: "raw_frames_001.txt".to_string(),
            size_bytes: 1024000,
            created_at: chrono::Utc::now().timestamp_millis(),
            file_type: "raw_frames".to_string(),
        }
    ];

    Ok(Json(ApiResponse {
        success: true,
        data: Some(files),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn download_file(Path(filename): Path<String>) -> Result<Response, StatusCode> {
    info!("File download requested: {}", filename);

    // TODO: 实现文件下载逻辑
    // 1. 验证文件名安全性
    // 2. 检查文件是否存在
    // 3. 返回文件内容

    // 暂时返回404
    Err(StatusCode::NOT_FOUND)
}

async fn save_waveform() -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("Waveform save requested");

    // TODO: 通过消息队列通知数据采集进程保存当前波形

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Waveform save initiated".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn health_check() -> Json<ApiResponse<String>> {
    Json(ApiResponse {
        success: true,
        data: Some("OK".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    })
}