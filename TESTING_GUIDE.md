# 数据采集系统测试指南

本文档详细描述了如何测试数据采集系统的各个组件和端到端功能。

## 测试环境准备

### 1. 系统要求检查

**操作系统**: Windows 10/11 (64位)

**必需软件**:
- GCC编译器 (MinGW-w64)
- Rust 1.70+
- Visual Studio Build Tools 2019/2022
- PowerShell 5.0+

### 2. 环境安装步骤

#### 2.1 安装MinGW-w64
```powershell
# 方法1: 使用winget
winget install mingw-w64

# 方法2: 手动下载
# 访问 https://www.mingw-w64.org/downloads/
# 下载并安装到 C:\mingw64
```

#### 2.2 安装Rust
```powershell
# 如果已安装，跳过此步骤
# 下载并运行 rustup-init.exe (项目目录中已有)
.\rustup-init.exe -y

# 重新加载环境变量
$env:PATH += ";$env:USERPROFILE\.cargo\bin"
```

#### 2.3 安装Visual Studio Build Tools
```powershell
# 下载Visual Studio Build Tools
# https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022

# 安装时确保选择:
# - C++ build tools
# - Windows 10/11 SDK
# - MSVC v143 compiler toolset
```

### 3. 验证环境
```powershell
# 检查GCC
gcc --version

# 检查Rust
cargo --version

# 检查链接器
where link.exe
```

## 阶段1: 基础组件测试

### 1.1 数据采集进程编译测试

```powershell
# 进入数据采集目录
cd data-reader

# 清理之前的编译文件
Remove-Item *.o, *.exe -ErrorAction SilentlyContinue

# 编译协议模块
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Iprotocol -O0 -g -c protocol/protocol.c -o protocol/protocol.o
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Iprotocol -O0 -g -c protocol/io_buffer.c -o protocol/io_buffer.o

# 编译共享内存模块
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Iprotocol -O0 -g -c shared_memory.c -o shared_memory.o

# 编译主程序
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Iprotocol -O0 -g -c serialread.c -o serialread.o

# 链接生成可执行文件
gcc serialread.o protocol/protocol.o protocol/io_buffer.o shared_memory.o -o serialread.exe -lkernel32 -luser32

# 验证编译成功
if (Test-Path "serialread.exe") {
    Write-Host "✓ 数据采集进程编译成功" -ForegroundColor Green
} else {
    Write-Host "✗ 数据采集进程编译失败" -ForegroundColor Red
    exit 1
}
```

### 1.2 数据采集进程功能测试

```powershell
# 测试帮助信息
.\serialread.exe --help

# 预期输出: 显示使用说明和命令列表
```

**预期结果**:
- 显示程序使用说明
- 列出可用的交互命令
- 程序正常退出 (退出码 0)

### 1.3 共享内存创建测试

```powershell
# 启动数据采集进程 (无串口连接)
Start-Process -FilePath ".\serialread.exe" -ArgumentList "999" -PassThru

# 检查共享内存是否创建
# 使用Process Explorer或任务管理器查看进程的内存映射
```

**预期结果**:
- 程序启动时显示 "Shared memory initialized successfully"
- 即使串口连接失败，共享内存仍应创建成功

## 阶段2: Rust数据处理进程测试

### 2.1 依赖检查和编译

```powershell
cd ..\data-processor

# 检查Cargo.toml配置
Get-Content Cargo.toml

# 下载依赖 (首次运行)
cargo fetch

# 语法检查
cargo check

# 编译 (Debug模式)
cargo build

# 编译 (Release模式)
cargo build --release
```

**故障排除**:
如果遇到链接器错误:
```powershell
# 确保Visual Studio Build Tools已安装
# 重启PowerShell以刷新环境变量
# 或手动设置链接器路径
$env:LINK = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.xx.xxxxx\bin\Hostx64\x64\link.exe"
```

### 2.2 配置文件测试

```powershell
# 测试配置加载
cargo run --bin data-processor -- --help

# 检查默认配置
cargo test config_tests
```

### 2.3 模块单元测试

```powershell
# 运行所有单元测试
cargo test

# 运行特定模块测试
cargo test ipc
cargo test data_processing
cargo test websocket
```

## 阶段3: 进程间通信测试

### 3.1 共享内存连接测试

**步骤1: 启动数据采集进程**
```powershell
# 终端1
cd data-reader
.\serialread.exe 999  # 使用不存在的COM端口，但会创建共享内存
```

**步骤2: 启动数据处理进程**
```powershell
# 终端2
cd data-processor
cargo run --release
```

**预期结果**:
- 数据采集进程显示: "Shared memory initialized successfully"
- 数据处理进程显示: "Connected to shared memory"
- 两个进程都正常运行，无错误退出

### 3.2 数据传输测试

**使用模拟数据测试**:

创建测试脚本 `test_data_flow.ps1`:
```powershell
# 创建模拟ADC数据
$testData = @(0x12, 0x34, 0x56, 0x78, 0xAB, 0xCD, 0xEF, 0x01)

# 这里需要实现一个简单的数据注入工具
# 或者修改serialread.c添加测试模式
```

## 阶段4: Web服务测试

### 4.1 WebSocket服务器测试

**使用浏览器测试**:
```javascript
// 在浏览器控制台中运行
const ws = new WebSocket('ws://localhost:8080');

ws.onopen = function(event) {
    console.log('WebSocket连接已建立');
    ws.send(JSON.stringify({type: 'subscribe'}));
};

ws.onmessage = function(event) {
    console.log('收到数据:', JSON.parse(event.data));
};

ws.onerror = function(error) {
    console.log('WebSocket错误:', error);
};
```

**使用PowerShell测试**:
```powershell
# 安装WebSocket客户端模块
Install-Module -Name WebSocketClient -Force

# 连接测试
$ws = New-WebSocketConnection -Uri "ws://localhost:8080"
Send-WebSocketMessage -Connection $ws -Message '{"type":"ping"}'
```

### 4.2 HTTPS API测试

```powershell
# 测试健康检查
Invoke-RestMethod -Uri "http://localhost:8443/health" -Method GET

# 测试系统状态
Invoke-RestMethod -Uri "http://localhost:8443/api/control/status" -Method GET

# 测试开始采集
Invoke-RestMethod -Uri "http://localhost:8443/api/control/start" -Method POST

# 测试停止采集
Invoke-RestMethod -Uri "http://localhost:8443/api/control/stop" -Method POST

# 测试文件列表
Invoke-RestMethod -Uri "http://localhost:8443/api/files" -Method GET
```

## 阶段5: 端到端集成测试

### 5.1 完整数据流测试

**测试场景**: 模拟真实的数据采集流程

```powershell
# 步骤1: 启动系统测试脚本
.\test_system.ps1

# 步骤2: 启动数据采集进程
cd data-reader
Start-Process -FilePath ".\serialread.exe" -ArgumentList "7" -PassThru

# 步骤3: 启动数据处理进程
cd ..\data-processor
Start-Process -FilePath "cargo" -ArgumentList "run --release" -PassThru

# 步骤4: 连接WebSocket客户端
# (使用浏览器或专用工具)

# 步骤5: 发送API命令
Invoke-RestMethod -Uri "http://localhost:8443/api/control/start" -Method POST
```

### 5.2 性能测试

**内存使用监控**:
```powershell
# 监控进程内存使用
Get-Process serialread | Select-Object Name, WorkingSet, VirtualMemorySize
Get-Process data-processor | Select-Object Name, WorkingSet, VirtualMemorySize

# 监控共享内存使用
# 使用Process Explorer查看内存映射详情
```

**数据吞吐量测试**:
```powershell
# 记录开始时间
$startTime = Get-Date

# 运行测试一段时间 (例如5分钟)
Start-Sleep -Seconds 300

# 检查处理的数据包数量
# (通过API获取统计信息)
$stats = Invoke-RestMethod -Uri "http://localhost:8443/api/control/status" -Method GET
Write-Host "处理的数据包数量: $($stats.data.packets_processed)"
```

## 阶段6: 错误处理和恢复测试

### 6.1 进程崩溃恢复测试

```powershell
# 测试数据采集进程崩溃后的恢复
Stop-Process -Name "serialread" -Force
Start-Sleep -Seconds 5
# 重新启动并检查共享内存状态

# 测试数据处理进程崩溃后的恢复
Stop-Process -Name "data-processor" -Force
Start-Sleep -Seconds 5
# 重新启动并检查连接状态
```

### 6.2 网络连接测试

```powershell
# 测试WebSocket连接断开重连
# 测试大量并发连接
# 测试网络延迟对性能的影响
```

## 测试结果记录

### 测试报告模板

创建 `test_results.md`:
```markdown
# 测试结果报告

## 测试环境
- 操作系统: Windows 11 Pro
- 处理器: Intel i7-xxxx
- 内存: 16GB
- 测试日期: 2024-xx-xx

## 测试结果

### 基础组件测试
- [ ] 数据采集进程编译: ✓/✗
- [ ] 数据处理进程编译: ✓/✗
- [ ] 共享内存创建: ✓/✗

### 功能测试
- [ ] WebSocket连接: ✓/✗
- [ ] HTTPS API: ✓/✗
- [ ] 数据处理: ✓/✗

### 性能测试
- 内存使用: xxx MB
- CPU使用率: xx%
- 数据吞吐量: xxx packets/sec

### 问题记录
1. 问题描述
   - 解决方案
2. ...
```

## 常见问题排查

### 编译问题
- **链接器错误**: 确保Visual Studio Build Tools正确安装
- **依赖缺失**: 运行 `cargo fetch` 重新下载依赖
- **路径问题**: 检查环境变量PATH设置

### 运行时问题
- **共享内存访问失败**: 检查进程权限，以管理员身份运行
- **端口占用**: 使用 `netstat -an` 检查端口使用情况
- **WebSocket连接失败**: 检查防火墙设置

### 性能问题
- **内存泄漏**: 使用Process Monitor监控内存使用
- **CPU占用过高**: 检查数据处理算法效率
- **网络延迟**: 使用网络监控工具分析

## 自动化测试脚本

创建 `run_all_tests.ps1`:
```powershell
# 完整的自动化测试脚本
param(
    [switch]$SkipBuild,
    [switch]$Verbose
)

Write-Host "=== 数据采集系统自动化测试 ===" -ForegroundColor Green

# 阶段1: 环境检查
Write-Host "检查测试环境..." -ForegroundColor Yellow
if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Write-Host "✗ GCC未安装" -ForegroundColor Red
    exit 1
}

if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    Write-Host "✗ Rust/Cargo未安装" -ForegroundColor Red
    exit 1
}

# 阶段2: 编译测试
if (-not $SkipBuild) {
    Write-Host "编译数据采集进程..." -ForegroundColor Yellow
    cd data-reader
    # 编译命令...
    cd ..

    Write-Host "编译数据处理进程..." -ForegroundColor Yellow
    cd data-processor
    cargo build --release
    cd ..
}

# 阶段3: 功能测试
Write-Host "运行功能测试..." -ForegroundColor Yellow
# 测试命令...

# 阶段4: 集成测试
Write-Host "运行集成测试..." -ForegroundColor Yellow
# 集成测试命令...

Write-Host "所有测试完成！" -ForegroundColor Green
```

通过以上详细的测试步骤，您可以系统性地验证整个数据采集系统的功能和性能。建议按照阶段顺序执行测试，确保每个阶段都通过后再进行下一阶段。