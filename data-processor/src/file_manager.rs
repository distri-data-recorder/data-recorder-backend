use anyhow::{anyhow, Result};
use chrono::Utc;
use serde::{Deserialize, Serialize};
use std::fs;
use std::io::Read;
use std::path::{Component, Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileInfo {
    pub filename: String,   // 相对 base 的路径（包含子目录时形如 "dir/name.ext"）
    pub size_bytes: u64,
    pub created_at: i64,
    pub file_type: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessedDataFile {
    pub filename: String,   // 仅文件名（不含目录）
    pub bytes: Vec<u8>,
}

pub struct FileManager {
    base: PathBuf,
}

impl FileManager {
    pub fn new<P: AsRef<Path>>(data_directory: P) -> Result<Self> {
        let p = data_directory.as_ref().to_path_buf();
        if !p.exists() {
            fs::create_dir_all(&p)
                .map_err(|e| anyhow!("create data dir {:?} failed: {}", &p, e))?;
        }
        Ok(Self { base: p })
    }

    /// 列出 base 或指定子目录下的文件（不递归）
    pub fn list_files_in(&self, rel_dir: Option<&str>) -> Result<Vec<FileInfo>> {
        let dir_path = if let Some(rd) = rel_dir {
            let safe = Self::sanitize_rel_path(rd)?;
            self.safe_join(&safe, true)?
        } else {
            self.base.clone()
        };

        let mut out = Vec::new();
        for entry in fs::read_dir(&dir_path)? {
            let entry = entry?;
            let path = entry.path();
            if let Some(info) = self.get_file_info(&path)? {
                out.push(info);
            }
        }
        out.sort_by(|a, b| b.created_at.cmp(&a.created_at));
        Ok(out)
    }

    /// 兼容旧接口：列出 base 目录下文件（不递归）
    pub fn list_files(&self) -> Result<Vec<FileInfo>> {
        self.list_files_in(None)
    }

    /// 读取相对 base 的文件（支持子目录）
    pub fn read_file(&self, rel_path: &str) -> Result<Vec<u8>> {
        let full = self.safe_join(&Self::sanitize_rel_path(rel_path)?, false)?;
        let mut f = fs::File::open(&full)?;
        let mut buf = Vec::new();
        f.read_to_end(&mut buf)?;
        Ok(buf)
    }

    /// 保存到 base 根目录（兼容旧接口）
    #[allow(dead_code)]
    pub fn save_processed_data(&self, data: &ProcessedDataFile) -> Result<String> {
        self.save_at(None, data)
    }

    /// 保存到子目录（相对 base）。返回相对路径："dir/filename" 或 "filename"
    pub fn save_at(&self, rel_dir: Option<&str>, data: &ProcessedDataFile) -> Result<String> {
        // 1) 目录
        let dir_path = if let Some(d) = rel_dir {
            let safe = Self::sanitize_rel_path(d)?;
            self.safe_join(&safe, true)?
        } else {
            self.base.clone()
        };
        fs::create_dir_all(&dir_path)?;

        // 2) 文件名
        let fname_safe = Self::sanitize_rel_path(&data.filename)?;
        if fname_safe.components().count() != 1 {
            return Err(anyhow!("filename must not contain path separators"));
        }

        // 3) 写入
        let full_path = dir_path.join(&fname_safe);
        fs::write(&full_path, &data.bytes)?;

        // 4) 返回相对路径
        let rel = if let Some(d) = rel_dir {
            let d_trim = d.trim_matches(|c| c == '/' || c == '\\');
            if d_trim.is_empty() {
                data.filename.clone()
            } else {
                format!("{}/{}", d_trim.replace('\\', "/"), data.filename)
            }
        } else {
            data.filename.clone()
        };
        Ok(rel)
    }

    /// 全局限额清理（仅 base 根目录；如需递归清理可按需扩展）
    pub fn cleanup_old_files(&self, max_files: usize) -> Result<()> {
        let mut files = self.list_files()?;
        if files.len() > max_files {
            files.drain(max_files..).for_each(|fi| {
                let _ = fs::remove_file(self.base.join(fi.filename));
            });
        }
        Ok(())
    }

    fn get_file_info(&self, path: &Path) -> Result<Option<FileInfo>> {
        if !path.is_file() {
            return Ok(None);
        }
        let meta = path.metadata()?;
        let size = meta.len();
        // created 在某些 FS 上可能不支持，退化成修改时间/当前时间
        let created = meta
            .created()
            .or_else(|_| meta.modified())
            .map(|t| {
                t.duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_millis() as i64
            })
            .unwrap_or_else(|_| Utc::now().timestamp_millis());

        // 生成相对 base 的字符串路径
        let rel = pathdiff::diff_paths(path, &self.base)
            .unwrap_or_else(|| PathBuf::from(path.file_name().unwrap_or_default()))
            .to_string_lossy()
            .replace('\\', "/");

        let file_type = Self::determine_file_type(rel.as_str());

        Ok(Some(FileInfo {
            filename: rel,
            size_bytes: size,
            created_at: created,
            file_type,
        }))
    }

    fn determine_file_type(filename: &str) -> String {
        let lower = filename.to_ascii_lowercase();
        if lower.ends_with(".txt") {
            "raw_frames".to_string()
        } else if lower.ends_with(".bin") || lower.ends_with(".dat") {
            "binary".to_string()
        } else if lower.ends_with(".json") {
            "json".to_string()
        } else {
            "unknown".to_string()
        }
    }

    /// 将相对路径拼到 base 上，并保证**不逃逸出 base**
    fn safe_join(&self, rel: &Path, ensure_dir: bool) -> Result<PathBuf> {
        let full = self.base.join(rel);
        // ensure_dir=true 时可不存在（用于创建目录），只校验父目录不逃逸
        let check_path = if ensure_dir { full.parent().unwrap_or(&full) } else { &full };

        // 使用标准化组件进行"软"校验
        let canon_base = self.base.canonicalize()?;
        let canon_check = check_path.canonicalize().unwrap_or_else(|_| check_path.to_path_buf());

        if !canon_check.starts_with(&canon_base) {
            return Err(anyhow!("path escapes base directory"));
        }
        Ok(full)
    }

    /// 仅允许相对路径；禁止 `..`、盘符、绝对路径、根目录
    fn sanitize_rel_path(input: &str) -> Result<PathBuf> {
        let p = Path::new(input);
        if p.is_absolute() {
            return Err(anyhow!("absolute path not allowed"));
        }
        let mut out = PathBuf::new();
        for comp in p.components() {
            match comp {
                Component::Normal(os) => {
                    let s = os.to_string_lossy();
                    if s.contains(':') {
                        return Err(anyhow!("path contains drive letter"));
                    }
                    // 简单长度限制，避免过长
                    if s.len() > 255 {
                        return Err(anyhow!("path component too long"));
                    }
                    out.push(os);
                }
                Component::CurDir => {} // 忽略 "."
                Component::ParentDir => return Err(anyhow!(".. is not allowed")),
                Component::RootDir | Component::Prefix(_) => {
                    return Err(anyhow!("root/prefix not allowed"))
                }
            }
        }
        Ok(out)
    }
}
