use anyhow::Result;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    pub shared_memory_name: String,
    pub message_queue_name: String,
    pub web_server: WebServerConfig,
    pub websocket: WebSocketConfig,
    pub data_processing: DataProcessingConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WebServerConfig {
    pub host: String,
    pub port: u16,
    pub tls_cert_path: Option<String>,
    pub tls_key_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WebSocketConfig {
    pub host: String,
    pub port: u16,
    pub max_connections: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataProcessingConfig {
    pub buffer_size: usize,
    pub processing_interval_ms: u64,
    pub max_packet_age_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            shared_memory_name: "ADC_DATA_SHARED_MEM".to_string(),
            message_queue_name: "data_reader_to_processor".to_string(),
            web_server: WebServerConfig {
                host: "127.0.0.1".to_string(),
                port: 8443,
                tls_cert_path: None,
                tls_key_path: None,
            },
            websocket: WebSocketConfig {
                host: "127.0.0.1".to_string(),
                port: 8080,
                max_connections: 100,
            },
            data_processing: DataProcessingConfig {
                buffer_size: 1024,
                processing_interval_ms: 10,
                max_packet_age_ms: 1000,
            },
        }
    }
}

impl Config {
    pub fn load() -> Result<Self> {
        // For now, use default configuration
        // In the future, this could load from a config file or environment variables
        Ok(Self::default())
    }
}