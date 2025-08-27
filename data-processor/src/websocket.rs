use crate::data_processing::ProcessedData;
use crate::device_communication::TriggerEvent;
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
    trigger_receiver: broadcast::Receiver<TriggerEvent>,  // 新增：触发事件接收器
    pub client_count_rx: watch::Receiver<usize>,
    client_count_tx: watch::Sender<usize>,
}

struct ClientConnection {
    sender: mpsc::UnboundedSender<Message>,
    subscriptions: ClientSubscriptions,  // 新增：客户端订阅管理
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
struct ClientSubscriptions {
    data_stream: bool,           // 是否订阅数据流
    trigger_events: bool,        // 是否订阅触发事件
    continuous_only: bool,       // 仅订阅连续数据
    trigger_only: bool,          // 仅订阅触发数据
}

impl Default for ClientSubscriptions {
    fn default() -> Self {
        Self {
            data_stream: true,       // 默认订阅数据流
            trigger_events: true,    // 默认订阅触发事件
            continuous_only: false,  // 默认不限制数据类型
            trigger_only: false,
        }
    }
}

impl WebSocketServer {
    pub fn new(
        config: WebSocketConfig, 
        data_receiver: broadcast::Receiver<ProcessedData>,
        trigger_receiver: broadcast::Receiver<TriggerEvent>,
    ) -> Self {
        let clients = Arc::new(RwLock::new(HashMap::new()));
        let (tx, rx) = watch::channel(0usize);
        Self {
            config,
            clients,
            data_receiver,
            trigger_receiver,
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

        // 触发事件广播 task
        let clients_clone2 = Arc::clone(&self.clients);
        let mut trigger_rx = self.trigger_receiver.resubscribe();
        tokio::spawn(async move {
            while let Ok(trigger_event) = trigger_rx.recv().await {
                Self::broadcast_trigger_event(&clients_clone2, &trigger_event).await;
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
                ClientConnection { 
                    sender: tx.clone(),
                    subscriptions: ClientSubscriptions::default(),
                },
            );
            let _ = client_count_tx.send(g.len());
        }

        info!("Client {} connected", client_id);

        // 欢迎消息
        let welcome = serde_json::json!({
            "type": "welcome",
            "client_id": client_id,
            "timestamp": chrono::Utc::now().timestamp_millis(),
            "server_capabilities": {
                "data_streaming": true,
                "trigger_events": true,
                "subscription_control": true
            }
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
        let clients_for_receiver = Arc::clone(&clients);
        let client_id_for_receiver = client_id.clone();
        let receiver_task = tokio::spawn(async move {
            while let Some(msg) = ws_receiver.next().await {
                match msg {
                    Ok(Message::Text(text)) => {
                        debug!("Client {} -> {}", client_id_for_receiver, text);
                        if let Err(e) = Self::handle_client_message(&client_id_for_receiver, &text, &clients_for_receiver).await {
                            warn!("handle_client_message error: {}", e);
                        }
                    }
                    Ok(Message::Close(_)) => {
                        info!("Client {} requested close", client_id_for_receiver);
                        break;
                    }
                    Ok(Message::Ping(_)) | Ok(Message::Pong(_)) => {
                        debug!("Ping/Pong {}", client_id_for_receiver);
                    }
                    Ok(Message::Binary(_)) => {
                        warn!("Binary message ignored from {}", client_id_for_receiver);
                    }
                    Ok(Message::Frame(_)) => {
                        debug!("Frame from {}", client_id_for_receiver);
                    }
                    Err(e) => {
                        error!("WebSocket error for {}: {}", client_id_for_receiver, e);
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

    async fn handle_client_message(
        client_id: &str,
        message: &str,
        clients: &Arc<RwLock<HashMap<String, ClientConnection>>>,
    ) -> Result<()> {
        // 解析客户端消息，支持订阅控制
        if let Ok(msg) = serde_json::from_str::<serde_json::Value>(message) {
            if let Some(msg_type) = msg.get("type").and_then(|v| v.as_str()) {
                match msg_type {
                    "subscribe" => {
                        // 处理订阅请求
                        if let Some(channels) = msg.get("channels").and_then(|v| v.as_array()) {
                            let mut g = clients.write().await;
                            if let Some(client) = g.get_mut(client_id) {
                                // 重置订阅状态
                                client.subscriptions = ClientSubscriptions {
                                    data_stream: false,
                                    trigger_events: false,
                                    continuous_only: false,
                                    trigger_only: false,
                                };

                                // 根据请求设置订阅
                                for channel in channels {
                                    if let Some(channel_str) = channel.as_str() {
                                        match channel_str {
                                            "data" => client.subscriptions.data_stream = true,
                                            "triggers" => client.subscriptions.trigger_events = true,
                                            "continuous_only" => {
                                                client.subscriptions.data_stream = true;
                                                client.subscriptions.continuous_only = true;
                                            }
                                            "trigger_only" => {
                                                client.subscriptions.data_stream = true;
                                                client.subscriptions.trigger_only = true;
                                            }
                                            _ => {}
                                        }
                                    }
                                }

                                info!("Client {} updated subscriptions: {:?}", client_id, client.subscriptions);

                                // 发送确认
                                let response = serde_json::json!({
                                    "type": "subscription_updated",
                                    "client_id": client_id,
                                    "subscriptions": client.subscriptions,
                                    "timestamp": chrono::Utc::now().timestamp_millis()
                                });
                                if let Ok(text) = serde_json::to_string(&response) {
                                    let _ = client.sender.send(Message::Text(text));
                                }
                            }
                        }
                    }
                    "ping" => {
                        // 处理客户端ping
                        let g = clients.read().await;
                        if let Some(client) = g.get(client_id) {
                            let pong = serde_json::json!({
                                "type": "pong",
                                "timestamp": chrono::Utc::now().timestamp_millis()
                            });
                            if let Ok(text) = serde_json::to_string(&pong) {
                                let _ = client.sender.send(Message::Text(text));
                            }
                        }
                    }
                    _ => {
                        debug!("Unknown message type from client {}: {}", client_id, msg_type);
                    }
                }
            }
        }
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
            "metadata": data.metadata,
            "data_type": data.data_type
        });

        if let Ok(text) = serde_json::to_string(&payload) {
            let g = clients.read().await;
            let mut drop_ids: Vec<String> = Vec::new();
            
            for (id, client) in g.iter() {
                // 检查客户端是否订阅了数据流
                if !client.subscriptions.data_stream {
                    continue;
                }

                // 检查数据类型过滤
                let should_send = match &data.data_type.source {
                    crate::data_processing::DataSource::Continuous => {
                        !client.subscriptions.trigger_only
                    }
                    crate::data_processing::DataSource::Trigger => {
                        !client.subscriptions.continuous_only
                    }
                };

                if should_send {
                    if client.sender.send(Message::Text(text.clone())).is_err() {
                        drop_ids.push(id.clone());
                    }
                }
            }
            // 保留注释：sender_task 会在发送失败时清理，这里显式 drop 掉临时变量以消告警
            drop(drop_ids);
        }
    }

    async fn broadcast_trigger_event(
        clients: &Arc<RwLock<HashMap<String, ClientConnection>>>,
        trigger_event: &TriggerEvent,
    ) {
        let payload = serde_json::json!({
            "type": "trigger_event",
            "timestamp": trigger_event.timestamp,
            "channel": trigger_event.channel,
            "pre_samples": trigger_event.pre_samples,
            "post_samples": trigger_event.post_samples,
            "event_time": chrono::Utc::now().timestamp_millis()
        });

        if let Ok(text) = serde_json::to_string(&payload) {
            let g = clients.read().await;
            let mut drop_ids: Vec<String> = Vec::new();
            
            for (id, client) in g.iter() {
                // 只发送给订阅了触发事件的客户端
                if client.subscriptions.trigger_events {
                    if client.sender.send(Message::Text(text.clone())).is_err() {
                        drop_ids.push(id.clone());
                    }
                }
            }
            drop(drop_ids);
        }

        info!(
            "Broadcasted trigger event to clients: ts={}, ch={}", 
            trigger_event.timestamp, trigger_event.channel
        );
    }
}