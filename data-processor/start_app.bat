@echo off

echo Setting up environment variables...

REM Set log level
set RUST_LOG=debug

REM --- Device Configuration ---
set DEVICE_TYPE=socket
set SOCKET_ADDRESS=127.0.0.1:9001
REM set SERIAL_PORT=COM7
set BAUD_RATE=115200

REM --- Web Server Configuration ---
set WEB_HOST=127.0.0.1
set WEB_PORT=8080

REM --- WebSocket Configuration ---
set WS_HOST=127.0.0.1
set WS_PORT=8081

REM --- File Storage Configuration ---
set DATA_DIR=./data
set FILE_PREFIX=wave
set FILE_EXT=.bin
set MAX_FILES=200

echo Environment variables set. Starting application...
cargo run