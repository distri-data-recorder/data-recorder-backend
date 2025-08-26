use anyhow::Result;
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct WebServerConfig {
    pub host: String,
    pub port: u16,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct WebSocketConfig {
    pub host: String,
    pub port: u16,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StorageConfig {
    /// 保存文件的根目录（启动时会确保存在）
    pub data_dir: String,
    /// 自动命名时的文件名前缀（如 "wave"）
    pub default_prefix: String,
    /// 自动命名时的扩展名（如 ".bin" / ".txt"）
    pub default_ext: String,
    /// 根目录最大保留文件数（超过后删除较旧文件）
    pub max_files: usize,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Config {
    pub web_server: WebServerConfig,
    pub websocket: WebSocketConfig,
    pub shared_memory_name: String,
    pub storage: StorageConfig,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            web_server: WebServerConfig {
                host: "127.0.0.1".into(),
                port: 8080,
            },
            websocket: WebSocketConfig {
                host: "127.0.0.1".into(),
                port: 8081, // 和 HTTP 区分开也可以
            },
            shared_memory_name: "ADC_DATA_SHARED_MEM".into(),
            storage: StorageConfig {
                data_dir: "./data".into(),
                default_prefix: "wave".into(),
                default_ext: ".bin".into(),
                max_files: 200,
            },
        }
    }
}

impl Config {
    /// 载入配置：默认值 + 环境变量覆盖
    ///
    /// 支持的环境变量：
    /// - WEB_HOST, WEB_PORT
    /// - WS_HOST, WS_PORT
    /// - SHM_NAME
    /// - DATA_DIR, FILE_PREFIX, FILE_EXT, MAX_FILES
    pub fn load() -> Result<Self> {
        let mut cfg = Self::default();

        // Web
        if let Ok(v) = std::env::var("WEB_HOST") {
            cfg.web_server.host = v;
        }
        if let Ok(v) = std::env::var("WEB_PORT") {
            if let Ok(p) = v.parse::<u16>() {
                cfg.web_server.port = p;
            }
        }

        // WebSocket
        if let Ok(v) = std::env::var("WS_HOST") {
            cfg.websocket.host = v;
        }
        if let Ok(v) = std::env::var("WS_PORT") {
            if let Ok(p) = v.parse::<u16>() {
                cfg.websocket.port = p;
            }
        }

        // Shared Memory
        if let Ok(v) = std::env::var("SHM_NAME") {
            cfg.shared_memory_name = v;
        }

        // Storage
        if let Ok(v) = std::env::var("DATA_DIR") {
            cfg.storage.data_dir = v;
        }
        if let Ok(v) = std::env::var("FILE_PREFIX") {
            cfg.storage.default_prefix = v;
        }
        if let Ok(v) = std::env::var("FILE_EXT") {
            // 自动补 '.' 前缀
            cfg.storage.default_ext = if v.starts_with('.') { v } else { format!(".{v}") };
        }
        if let Ok(v) = std::env::var("MAX_FILES") {
            if let Ok(n) = v.parse::<usize>() {
                cfg.storage.max_files = n;
            }
        }

        Ok(cfg)
    }
}
