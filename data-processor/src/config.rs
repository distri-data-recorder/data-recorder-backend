use anyhow::Result;
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DeviceConfig {
    pub connection_type: String, // "serial" or "socket"
    pub serial_port: Option<String>,
    pub socket_address: Option<String>,
    pub baud_rate: u32,
}

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
    pub device: DeviceConfig,
    pub web_server: WebServerConfig,
    pub websocket: WebSocketConfig,
    pub storage: StorageConfig,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            device: DeviceConfig {
                connection_type: "socket".into(),
                serial_port: Some("COM7".into()),
                socket_address: Some("127.0.0.1:9001".into()),
                baud_rate: 115200,
            },
            web_server: WebServerConfig {
                host: "127.0.0.1".into(),
                port: 8080,
            },
            websocket: WebSocketConfig {
                host: "127.0.0.1".into(),
                port: 8081,
            },
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
    /// - DEVICE_TYPE, SERIAL_PORT, SOCKET_ADDRESS, BAUD_RATE
    /// - WEB_HOST, WEB_PORT
    /// - WS_HOST, WS_PORT
    /// - DATA_DIR, FILE_PREFIX, FILE_EXT, MAX_FILES
    pub fn load() -> Result<Self> {
        let mut cfg = Self::default();

        // Device
        if let Ok(v) = std::env::var("DEVICE_TYPE") {
            cfg.device.connection_type = v;
        }
        if let Ok(v) = std::env::var("SERIAL_PORT") {
            cfg.device.serial_port = Some(v);
        }
        if let Ok(v) = std::env::var("SOCKET_ADDRESS") {
            cfg.device.socket_address = Some(v);
        }
        if let Ok(v) = std::env::var("BAUD_RATE") {
            if let Ok(rate) = v.parse::<u32>() {
                cfg.device.baud_rate = rate;
            }
        }

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