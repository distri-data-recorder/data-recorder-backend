use anyhow::{Result, anyhow};
use std::fs;
use std::path::{Path, PathBuf};
use tracing::{info, warn};

pub struct FileManager {
    data_directory: PathBuf,
}

impl FileManager {
    pub fn new<P: AsRef<Path>>(data_directory: P) -> Result<Self> {
        let data_dir = data_directory.as_ref().to_path_buf();

        // 确保数据目录存在
        if !data_dir.exists() {
            fs::create_dir_all(&data_dir)?;
            info!("Created data directory: {:?}", data_dir);
        }

        Ok(Self {
            data_directory: data_dir,
        })
    }

    pub fn list_files(&self) -> Result<Vec<FileInfo>> {
        let mut files = Vec::new();

        for entry in fs::read_dir(&self.data_directory)? {
            let entry = entry?;
            let path = entry.path();

            if path.is_file() {
                if let Some(file_info) = self.get_file_info(&path)? {
                    files.push(file_info);
                }
            }
        }

        // 按创建时间排序（最新的在前）
        files.sort_by(|a, b| b.created_at.cmp(&a.created_at));

        Ok(files)
    }

    pub fn get_file_path(&self, filename: &str) -> Result<PathBuf> {
        // 验证文件名安全性
        if filename.contains("..") || filename.contains("/") || filename.contains("\\") {
            return Err(anyhow!("Invalid filename: {}", filename));
        }

        let file_path = self.data_directory.join(filename);

        if !file_path.exists() {
            return Err(anyhow!("File not found: {}", filename));
        }

        Ok(file_path)
    }

    pub fn read_file(&self, filename: &str) -> Result<Vec<u8>> {
        let file_path = self.get_file_path(filename)?;
        let content = fs::read(&file_path)?;

        info!("Read file: {} ({} bytes)", filename, content.len());
        Ok(content)
    }

    pub fn save_processed_data(&self, data: &ProcessedDataFile) -> Result<String> {
        let timestamp = chrono::Utc::now().format("%Y%m%d_%H%M%S");
        let filename = format!("processed_data_{}.json", timestamp);
        let file_path = self.data_directory.join(&filename);

        let json_content = serde_json::to_string_pretty(data)?;
        fs::write(&file_path, json_content)?;

        info!("Saved processed data to: {}", filename);
        Ok(filename)
    }

    pub fn cleanup_old_files(&self, max_files: usize) -> Result<()> {
        let mut files = self.list_files()?;

        if files.len() <= max_files {
            return Ok(());
        }

        // 删除最旧的文件
        files.sort_by(|a, b| a.created_at.cmp(&b.created_at));
        let files_to_delete = files.len() - max_files;

        for file in files.iter().take(files_to_delete) {
            let file_path = self.data_directory.join(&file.filename);
            if let Err(e) = fs::remove_file(&file_path) {
                warn!("Failed to delete old file {}: {}", file.filename, e);
            } else {
                info!("Deleted old file: {}", file.filename);
            }
        }

        Ok(())
    }

    fn get_file_info(&self, path: &Path) -> Result<Option<FileInfo>> {
        let metadata = fs::metadata(path)?;

        if let Some(filename) = path.file_name().and_then(|n| n.to_str()) {
            let file_type = self.determine_file_type(filename);

            let created_at = metadata
                .created()
                .or_else(|_| metadata.modified())
                .unwrap_or(std::time::SystemTime::UNIX_EPOCH)
                .duration_since(std::time::SystemTime::UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis() as i64;

            Ok(Some(FileInfo {
                filename: filename.to_string(),
                size_bytes: metadata.len(),
                created_at,
                file_type,
            }))
        } else {
            Ok(None)
        }
    }

    fn determine_file_type(&self, filename: &str) -> String {
        if filename.starts_with("raw_frames_") && filename.ends_with(".txt") {
            "raw_frames".to_string()
        } else if filename.starts_with("processed_data_") && filename.ends_with(".json") {
            "processed_data".to_string()
        } else {
            "unknown".to_string()
        }
    }
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct FileInfo {
    pub filename: String,
    pub size_bytes: u64,
    pub created_at: i64,
    pub file_type: String,
}

#[derive(Debug, serde::Serialize, serde::Deserialize)]
pub struct ProcessedDataFile {
    pub metadata: FileMetadata,
    pub data_packets: Vec<DataPacketRecord>,
}

#[derive(Debug, serde::Serialize, serde::Deserialize)]
pub struct FileMetadata {
    pub created_at: i64,
    pub version: String,
    pub sample_rate: f64,
    pub channel_count: usize,
    pub total_packets: usize,
    pub duration_ms: u64,
}

#[derive(Debug, serde::Serialize, serde::Deserialize)]
pub struct DataPacketRecord {
    pub timestamp: u64,
    pub sequence: u16,
    pub data: Vec<f64>,
    pub quality: String,
}