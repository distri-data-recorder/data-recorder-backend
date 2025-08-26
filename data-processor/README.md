---

# data-processor

Rust 实现的数据处理后台，负责从 `data-reader` 共享内存读取原始帧、做基础整形并通过 WebSocket/HTTP 提供给前端；同时通过 Windows 命名管道与 `data-reader` 双向通信。

## 功能一览

* **共享内存读取**：读取 C 端 `data-reader` 写入的环形缓冲区（含头信息与 `ADCDataPacket` 数组）
* **数据广播**：处理后的数据以 JSON 通过 **WebSocket** 广播给所有订阅者
* **HTTP API**：

  * 采集控制：`/api/control/start|stop|request_status`、汇总状态 `/api/control/status`
  * 文件管理：列举 `/api/files`、下载 `/api/files/:filename`、保存 `/api/files/save`
* **IPC**：经命名管道（默认 `\\.\pipe\data_reader_ipc`）转发控制命令、接收状态/日志
* **可配置存储**：`DATA_DIR` 根目录；前端可在 `POST /api/files/save` 动态传 `dir`（相对子目录）与 `filename`
* **安全**：文件接口仅允许写入 `DATA_DIR` 下，拒绝绝对路径/盘符/`..` 等路径逃逸

---

## 环境要求

* Windows 10/11 x64
* Rust（`rustup` + **windows-gnu** 或 **windows-msvc** 工具链均可）
* 已编译好的 `data-reader`（串口或 socket 输入均可）

> 你已在本机完成编译，可跳过构建环节，直接看“启动与联调”。

---

## 配置

支持通过环境变量覆盖默认参数（如不设置则用默认值）：

| 变量名           | 含义                   | 默认值                   |
| ------------- | -------------------- | --------------------- |
| `WEB_HOST`    | HTTP 监听地址            | `127.0.0.1`           |
| `WEB_PORT`    | HTTP 端口              | `8080`                |
| `WS_HOST`     | WebSocket 监听地址       | `127.0.0.1`           |
| `WS_PORT`     | WebSocket 端口         | `8081`                |
| `SHM_NAME`    | 共享内存名                | `ADC_DATA_SHARED_MEM` |
| `DATA_DIR`    | 文件根目录                | `./data`              |
| `FILE_PREFIX` | 自动命名的文件名前缀           | `wave`                |
| `FILE_EXT`    | 自动命名扩展名              | `.bin`                |
| `MAX_FILES`   | `DATA_DIR` 根目录最多保留文件 | `200`                 |

PowerShell 示例（可按需设置）：

```powershell
$env:WEB_HOST="0.0.0.0"
$env:WEB_PORT="8080"
$env:WS_HOST="0.0.0.0"
$env:WS_PORT="8081"
$env:SHM_NAME="ADC_DATA_SHARED_MEM"
$env:DATA_DIR="D:\recorder\data"
$env:FILE_PREFIX="rec"
$env:FILE_EXT=".dat"
$env:MAX_FILES="500"
```

---

## 启动与联调

### 1) 启动 data-reader

* **串口模式**（例：COM7）

  ```powershell
  C:\path\to\serialread.exe 7
  ```
* **Socket 模式**（接收 test-sender，默认 `127.0.0.1:9001`，以你们文档为准）

  ```powershell
  C:\path\to\serialread.exe -s
  ```

> 确认 data-reader 正常运行后再启动 data-processor。它会创建共享内存与命名管道。

### 2) 启动 data-processor

* 发布版：

  ```powershell
  .\target\release\data-processor.exe
  ```

启动日志中可见：

* “Connected to shared memory”
* HTTP server on `WEB_HOST:WEB_PORT`
* WebSocket server on `WS_HOST:WS_PORT`

---

## 快速自测（HTTP）

下面用 `curl.exe` 演示（PowerShell 可直接执行）。

### 健康检查

```powershell
curl http://127.0.0.1:8080/health
```

### 查看汇总状态

```powershell
curl http://127.0.0.1:8080/api/control/status
```

### 请求 data-reader 状态（通过 IPC）

```powershell
curl -X POST http://127.0.0.1:8080/api/control/request_status
```

### 开始 / 停止采集

```powershell
curl -X POST http://127.0.0.1:8080/api/control/start
curl -X POST http://127.0.0.1:8080/api/control/stop
```

---

## WebSocket 数据流

使用 **wscat**（Node 工具）或浏览器连接：

```powershell
# 安装（如未安装）
npm i -g wscat

# 连接
wscat -c ws://127.0.0.1:8081
```

看到形如：

```json
{
  "type": "data",
  "timestamp": 1693045250123,
  "sequence": 123,
  "channel_count": 8,
  "sample_rate": 10000,
  "data": [ ... ],
  "metadata": { ... }
}
```

> 如果暂时没有数据，检查 data-reader 是否已连接设备或 test-sender 是否在推送。

---

## 文件接口自测

### 1) 保存（前端可自定义目录与文件名）

`POST /api/files/save`

* `dir`：相对 `DATA_DIR` 的子目录（可选）
* `filename`：文件名（可选；不传则自动命名）
* `base64`：文件内容的 Base64

**示例 A：只指定目录，自动命名**

```powershell
# 准备要保存的字节（示例：将 "hello world" 转为 base64）
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("hello world"))

# 提交
curl -H "Content-Type: application/json" -X POST http://127.0.0.1:8080/api/files/save `
  -d "{""dir"":""projectA/run_001"",""base64"":""$b64""}"
```

成功返回：

```json
{ "success": true, "data": "projectA/run_001/wave_YYYYMMDD_HHMMSS.bin", ... }
```

**示例 B：自定义目录 + 文件名**

```powershell
$b64 = [Convert]::ToBase64String([byte[]](1..10)) # 随机数据示例
curl -H "Content-Type: application/json" -X POST http://127.0.0.1:8080/api/files/save `
  -d "{""dir"":""projectA/run_002"",""filename"":""ch1_0001.dat"",""base64"":""$b64""}"
```

> 安全校验：仅允许相对路径；拒绝绝对路径/盘符/`..`。保存路径始终限制在 `DATA_DIR` 下。
> 超出 `MAX_FILES`（针对根目录）会自动清理旧文件（可按需拓展到递归清理）。

### 2) 列文件

* 根目录：

  ```powershell
  curl http://127.0.0.1:8080/api/files
  ```
* 子目录：

  ```powershell
  curl "http://127.0.0.1:8080/api/files?dir=projectA/run_001"
  ```

### 3) 下载文件（支持子目录）

```powershell
# 将文件保存为本地 out.bin
curl -o out.bin http://127.0.0.1:8080/api/files/projectA/run_001/ch1_0001.dat
```

---

## 与 test-sender 联调（可选）

若 `data-reader` 以 `-s`（socket）模式运行，按 test-sender 的 README 发送模拟帧到它监听的端口（默认 9001）。
此时 `data-processor` 的 WebSocket 客户端应收到持续的 `"type":"data"` 消息，`/api/control/status` 的 `packets_processed` 也会持续增长。

---

## 常见问题（FAQ）

* **编译/链接（GNU 工具链）**
  请确保使用 **MSYS2 mingw64** 或 `rustup` 自带的 `windows-gnu` 工具链。避免混用 w64devkit 导致 `-lgcc_eh` 等库缺失。
  推荐 `.cargo/config.toml` 使用：

  ```toml
  [build]
  target = "x86_64-pc-windows-gnu"

  [target.x86_64-pc-windows-gnu]
  linker = "x86_64-w64-mingw32-gcc"
  ar = "x86_64-w64-mingw32-gcc-ar"
  ```

* **启动顺序**
  先启动 `data-reader`（创建共享内存/管道），再启动 `data-processor`。否则会因无法连接共享内存/管道而退出或重试。

* **HTTP/WS 端口被占用**
  修改 `WEB_PORT` / `WS_PORT` 或关闭占用端口的程序。

* **保存文件失败**
  检查 `DATA_DIR` 是否可写；确认 `dir` 与 `filename` 未包含绝对路径、盘符或 `..`。

* **收不到数据**
  确认设备或 test-sender 正在向 `data-reader` 输入数据；查看 `data-processor` 日志中是否有 `IPC status`/`IPC frame`；检查共享内存名 `SHM_NAME` 是否一致。

---

## 开发者模式

* 调试运行：

  ```powershell
  $env:RUST_LOG="info"
  cargo run
  ```
* 一键修复部分告警：

  ```powershell
  cargo fix --bin data-processor
  ```

---

## 目录结构（关键文件）

```
src/
  main.rs            # 程序入口，任务编排、IPC 订阅日志
  ipc.rs             # 命名管道 JSON-Lines 客户端 + 共享内存读取
  data_processing.rs # 从共享内存拉帧，组装 ProcessedData 并广播
  websocket.rs       # WebSocket 广播服务
  web_server.rs      # HTTP API（控制 + 文件）
  file_manager.rs    # 文件安全存取（限定在 DATA_DIR 下）
  config.rs          # 配置载入（默认 + 环境变量）
```
