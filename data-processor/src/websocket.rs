use crate::config::WebSocketConfig;
use crate::data_processing::ProcessedData;
use anyhow::Result;
use futures_util::{SinkExt, StreamExt};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, RwLock};
use tokio_tungstenite::{accept_async, tungstenite::Message};
use tracing::{info, warn, error, debug};
use uuid::Uuid;

pub struct WebSocketServer {
    config: WebSocketConfig,
    clients: Arc<RwLock<HashMap<String, ClientConnection>>>,
    data_receiver: broadcast::Receiver<ProcessedData>,
}

struct ClientConnection {
    id: String,
    sender: tokio::sync::mpsc::UnboundedSender<Message>,
}

impl WebSocketServer {
    pub fn new(config: WebSocketConfig, data_receiver: broadcast::Receiver<ProcessedData>) -> Self {
        Self {
            config,
            clients: Arc::new(RwLock::new(HashMap::new())),
            data_receiver,
        }
    }

    pub async fn run(&mut self) -> Result<()> {
        let addr = format!("{}:{}", self.config.host, self.config.port);
        let listener = TcpListener::bind(&addr).await?;
        info!("WebSocket server listening on {}", addr);

        // 启动数据广播任务
        let clients_clone = Arc::clone(&self.clients);
        let mut data_rx = self.data_receiver.resubscribe();
        tokio::spawn(async move {
            while let Ok(data) = data_rx.recv().await {
                Self::broadcast_data(&clients_clone, &data).await;
            }
        });

        // 接受客户端连接
        while let Ok((stream, addr)) = listener.accept().await {
            info!("New WebSocket connection from {}", addr);

            let clients = Arc::clone(&self.clients);
            tokio::spawn(async move {
                if let Err(e) = Self::handle_connection(stream, clients).await {
                    error!("WebSocket connection error: {}", e);
                }
            });
        }

        Ok(())
    }

    async fn handle_connection(
        stream: TcpStream,
        clients: Arc<RwLock<HashMap<String, ClientConnection>>>,
    ) -> Result<()> {
        let ws_stream = accept_async(stream).await?;
        let (mut ws_sender, mut ws_receiver) = ws_stream.split();

        let client_id = Uuid::new_v4().to_string();
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();

        // 添加客户端到连接列表
        {
            let mut clients_guard = clients.write().await;
            clients_guard.insert(client_id.clone(), ClientConnection {
                id: client_id.clone(),
                sender: tx.clone(),
            });
        }

        info!("Client {} connected", client_id);

        // 发送欢迎消息
        let welcome_msg = serde_json::json!({
            "type": "welcome",
            "client_id": client_id,
            "timestamp": chrono::Utc::now().timestamp_millis()
        });

        if let Ok(msg_text) = serde_json::to_string(&welcome_msg) {
            let _ = tx.send(Message::Text(msg_text));
        }

        // 处理发送任务
        let clients_for_sender = Arc::clone(&clients);
        let client_id_for_sender = client_id.clone();
        let sender_task = tokio::spawn(async move {
            while let Some(message) = rx.recv().await {
                if let Err(e) = ws_sender.send(message).await {
                    error!("Failed to send message to client {}: {}", client_id_for_sender, e);
                    break;
                }
            }

            // 清理客户端连接
            let mut clients_guard = clients_for_sender.write().await;
            clients_guard.remove(&client_id_for_sender);
            info!("Client {} disconnected", client_id_for_sender);
        });

        // 处理接收任务
        let receiver_task = tokio::spawn(async move {
            while let Some(msg) = ws_receiver.next().await {
                match msg {
                    Ok(Message::Text(text)) => {
                        debug!("Received message from {}: {}", client_id, text);
                        // 处理客户端消息（如果需要）
                        if let Err(e) = Self::handle_client_message(&client_id, &text).await {
                            warn!("Error handling message from {}: {}", client_id, e);
                        }
                    }
                    Ok(Message::Close(_)) => {
                        info!("Client {} requested close", client_id);
                        break;
                    }
                    Ok(Message::Ping(_data)) => {
                        debug!("Ping from client {}", client_id);
                        // Pong will be sent automatically
                    }
                    Ok(Message::Pong(_)) => {
                        debug!("Pong from client {}", client_id);
                    }
                    Ok(Message::Binary(_)) => {
                        warn!("Received binary message from {}, ignoring", client_id);
                    }
                    Ok(Message::Frame(_)) => {
                        // Handle raw frame messages (usually not needed in application code)
                        debug!("Received frame message from {}", client_id);
                    }
                    Err(e) => {
                        error!("WebSocket error for client {}: {}", client_id, e);
                        break;
                    }
                }
            }
        });

        // 等待任一任务完成
        tokio::select! {
            _ = sender_task => {},
            _ = receiver_task => {},
        }

        Ok(())
    }

    async fn handle_client_message(client_id: &str, message: &str) -> Result<()> {
        // 解析客户端消息
        if let Ok(msg) = serde_json::from_str::<serde_json::Value>(message) {
            match msg.get("type").and_then(|t| t.as_str()) {
                Some("ping") => {
                    debug!("Ping from client {}", client_id);
                    // 可以发送pong响应
                }
                Some("subscribe") => {
                    info!("Client {} subscribed to data stream", client_id);
                    // 客户端订阅数据流
                }
                Some("unsubscribe") => {
                    info!("Client {} unsubscribed from data stream", client_id);
                    // 客户端取消订阅
                }
                _ => {
                    debug!("Unknown message type from client {}: {}", client_id, message);
                }
            }
        }

        Ok(())
    }

    async fn broadcast_data(
        clients: &Arc<RwLock<HashMap<String, ClientConnection>>>,
        data: &ProcessedData,
    ) {
        let message = serde_json::json!({
            "type": "data",
            "timestamp": data.timestamp,
            "sequence": data.sequence,
            "channel_count": data.channel_count,
            "sample_rate": data.sample_rate,
            "data": data.data,
            "metadata": data.metadata
        });

        if let Ok(msg_text) = serde_json::to_string(&message) {
            let clients_guard = clients.read().await;
            let mut failed_clients = Vec::new();

            for (client_id, client) in clients_guard.iter() {
                if let Err(_) = client.sender.send(Message::Text(msg_text.clone())) {
                    failed_clients.push(client_id.clone());
                }
            }

            // 清理失败的连接将在发送任务中处理
            if !failed_clients.is_empty() {
                debug!("Failed to send to {} clients", failed_clients.len());
            }
        }
    }
}