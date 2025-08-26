use crate::data_processing::ProcessedData;
use crate::config::WebSocketConfig;
use anyhow::Result;
use futures_util::{SinkExt, StreamExt};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, mpsc, RwLock, watch};
use tokio_tungstenite::{accept_async, tungstenite::Message};
use tracing::{debug, error, info, warn};
use uuid::Uuid;

pub struct WebSocketServer {
    config: WebSocketConfig,
    clients: Arc<RwLock<HashMap<String, ClientConnection>>>,
    data_receiver: broadcast::Receiver<ProcessedData>,
    pub client_count_rx: watch::Receiver<usize>,
    client_count_tx: watch::Sender<usize>,
}

struct ClientConnection {
    sender: mpsc::UnboundedSender<Message>,
}

impl WebSocketServer {
    pub fn new(config: WebSocketConfig, data_receiver: broadcast::Receiver<ProcessedData>) -> Self {
        let clients = Arc::new(RwLock::new(HashMap::new()));
        let (tx, rx) = watch::channel(0usize);
        Self {
            config,
            clients,
            data_receiver,
            client_count_rx: rx,
            client_count_tx: tx,
        }
    }

    pub async fn run(&mut self) -> Result<()> {
        let addr = format!("{}:{}", self.config.host, self.config.port);
        let listener = TcpListener::bind(&addr).await?;
        info!("WebSocket server listening on {}", addr);

        // 数据广播 task
        let clients_clone = Arc::clone(&self.clients);
        let mut data_rx = self.data_receiver.resubscribe();
        tokio::spawn(async move {
            while let Ok(data) = data_rx.recv().await {
                Self::broadcast_data(&clients_clone, &data).await;
            }
        });

        // 接受客户端连接
        loop {
            let (stream, addr) = listener.accept().await?;
            info!("New WebSocket connection from {}", addr);

            let clients = Arc::clone(&self.clients);
            let tx_count = self.client_count_tx.clone();
            tokio::spawn(async move {
                if let Err(e) = Self::handle_connection(stream, clients, tx_count).await {
                    error!("WebSocket connection error: {}", e);
                }
            });
        }
    }

    async fn handle_connection(
        stream: TcpStream,
        clients: Arc<RwLock<HashMap<String, ClientConnection>>>,
        client_count_tx: watch::Sender<usize>,
    ) -> Result<()> {
        let ws_stream = accept_async(stream).await?;
        let (mut ws_sender, mut ws_receiver) = ws_stream.split();

        let client_id = Uuid::new_v4().to_string();
        let (tx, mut rx) = mpsc::unbounded_channel();

        // 添加到连接表
        {
            let mut g = clients.write().await;
            g.insert(
                client_id.clone(),
                ClientConnection { sender: tx.clone() },
            );
            let _ = client_count_tx.send(g.len());
        }

        info!("Client {} connected", client_id);

        // 欢迎消息
        let welcome = serde_json::json!({
            "type": "welcome",
            "client_id": client_id,
            "timestamp": chrono::Utc::now().timestamp_millis()
        });
        if let Ok(t) = serde_json::to_string(&welcome) {
            let _ = tx.send(Message::Text(t));
        }

        // 发送任务
        let clients_for_sender = Arc::clone(&clients);
        let client_id_for_sender = client_id.clone();
        let client_count_tx_sender = client_count_tx.clone();
        let sender_task = tokio::spawn(async move {
            while let Some(msg) = rx.recv().await {
                if let Err(e) = ws_sender.send(msg).await {
                    error!(
                        "Failed to send message to client {}: {}",
                        client_id_for_sender, e
                    );
                    break;
                }
            }
            let mut g = clients_for_sender.write().await;
            g.remove(&client_id_for_sender);
            let _ = client_count_tx_sender.send(g.len());
            info!("Client {} disconnected", client_id_for_sender);
        });

        // 接收任务
        let receiver_task = tokio::spawn(async move {
            while let Some(msg) = ws_receiver.next().await {
                match msg {
                    Ok(Message::Text(text)) => {
                        debug!("Client {} -> {}", client_id, text);
                        if let Err(e) = Self::handle_client_message(&client_id, &text).await {
                            warn!("handle_client_message error: {}", e);
                        }
                    }
                    Ok(Message::Close(_)) => {
                        info!("Client {} requested close", client_id);
                        break;
                    }
                    Ok(Message::Ping(_)) | Ok(Message::Pong(_)) => {
                        debug!("Ping/Pong {}", client_id);
                    }
                    Ok(Message::Binary(_)) => {
                        warn!("Binary message ignored from {}", client_id);
                    }
                    Ok(Message::Frame(_)) => {
                        debug!("Frame from {}", client_id);
                    }
                    Err(e) => {
                        error!("WebSocket error for {}: {}", client_id, e);
                        break;
                    }
                }
            }
        });

        tokio::select! {
            _ = sender_task => {},
            _ = receiver_task => {},
        }
        Ok(())
    }

    async fn handle_client_message(_client_id: &str, _message: &str) -> Result<()> {
        // 可实现订阅过滤/控制指令等
        Ok(())
    }

    async fn broadcast_data(
        clients: &Arc<RwLock<HashMap<String, ClientConnection>>>,
        data: &ProcessedData,
    ) {
        let payload = serde_json::json!({
            "type": "data",
            "timestamp": data.timestamp,
            "sequence": data.sequence,
            "channel_count": data.channel_count,
            "sample_rate": data.sample_rate,
            "data": data.data,
            "metadata": data.metadata
        });

        if let Ok(text) = serde_json::to_string(&payload) {
            let g = clients.read().await;
            let mut drop_ids: Vec<String> = Vec::new();
            for (id, c) in g.iter() {
                if c.sender.send(Message::Text(text.clone())).is_err() {
                    drop_ids.push(id.clone());
                }
            }
            // 保留注释：sender_task 会在发送失败时清理，这里显式 drop 掉临时变量以消告警
            drop(drop_ids);
        }
    }
}
