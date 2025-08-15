mod ipc;
mod data_processing;
mod web_server;
mod websocket;
mod file_manager;
mod config;

use anyhow::Result;
use tracing::{info, warn, error};
use tracing_subscriber;

#[tokio::main]
async fn main() -> Result<()> {
    // Initialize logging
    tracing_subscriber::fmt::init();

    info!("Starting Data Processor Service");

    // Load configuration
    let config = config::Config::load()?;
    info!("Configuration loaded: {:?}", config);

    // Initialize shared memory connection (with fallback for development)
    let shared_mem = match ipc::SharedMemoryReader::new(&config.shared_memory_name) {
        Ok(mem) => {
            info!("Connected to shared memory");
            mem
        }
        Err(e) => {
            warn!("Failed to connect to shared memory: {}. Running in standalone mode.", e);
            // For development, we'll create a mock shared memory reader
            return Err(e);
        }
    };

    // Initialize message queue
    let _message_queue = ipc::MessageQueue::new(&config.message_queue_name)?;
    info!("Connected to message queue");

    // Start data processing task
    let mut data_processor = data_processing::DataProcessor::new(shared_mem);
    let data_receiver = data_processor.get_data_receiver();

    let processing_handle = tokio::spawn(async move {
        if let Err(e) = data_processor.run().await {
            error!("Data processing error: {}", e);
        }
    });

    // Start WebSocket server
    let mut websocket_server = websocket::WebSocketServer::new(
        config.websocket.clone(),
        data_receiver,
    );
    let websocket_handle = tokio::spawn(async move {
        if let Err(e) = websocket_server.run().await {
            error!("WebSocket server error: {}", e);
        }
    });

    // Start web server
    let web_server = web_server::WebServer::new(config.clone());
    let server_handle = tokio::spawn(async move {
        if let Err(e) = web_server.run().await {
            error!("Web server error: {}", e);
        }
    });

    info!("All services started successfully");

    // Wait for all tasks to complete
    tokio::select! {
        _ = processing_handle => {
            error!("Data processing task terminated");
        }
        _ = websocket_handle => {
            error!("WebSocket server task terminated");
        }
        _ = server_handle => {
            error!("Web server task terminated");
        }
        _ = tokio::signal::ctrl_c() => {
            info!("Received shutdown signal");
        }
    }

    info!("Shutting down Data Processor Service");
    Ok(())
}
