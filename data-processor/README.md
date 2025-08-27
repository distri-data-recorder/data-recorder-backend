# Data Processor

A high-performance data acquisition and processing system built in Rust, designed for real-time sensor data collection, processing, and distribution.

## Features

- **Direct Device Communication**: Supports both serial (USB-CDC) and TCP socket connections
- **Real-time Data Processing**: Filtering, quality assessment, and format conversion
- **WebSocket Streaming**: Live data distribution to multiple clients
- **REST API**: Comprehensive control and monitoring interface
- **File Management**: Secure data storage with automatic cleanup
- **Cross-platform**: Windows, Linux, and macOS support

## Quick Start

### Installation

```bash
# Clone the repository
git clone <repository-url>
cd data-processor

# Build the application
cargo build --release

# Run with default configuration
cargo run --release
```

### Configuration

Configure via environment variables:

```bash
# Device connection
export DEVICE_TYPE=socket           # "serial" or "socket"
export SOCKET_ADDRESS=127.0.0.1:9001  # For socket mode
export SERIAL_PORT=COM7             # For serial mode
export BAUD_RATE=115200

# Web services
export WEB_HOST=127.0.0.1
export WEB_PORT=8080
export WS_HOST=127.0.0.1
export WS_PORT=8081

# Storage
export DATA_DIR=./data
export FILE_PREFIX=wave
export FILE_EXT=.bin
export MAX_FILES=200
```

### Running with Device Simulator

```bash
# Terminal 1: Start device simulator
cd device-simulator-v2.1
make run

# Terminal 2: Start data processor
cd data-processor  
cargo run --release
```

Access the system:
- Web API: http://127.0.0.1:8080
- WebSocket: ws://127.0.0.1:8081

## API Documentation

### System Status

#### Get System Status
```http
GET /api/control/status
```

**Response:**
```json
{
  "success": true,
  "data": {
    "data_collection_active": false,
    "device_connected": true,
    "connected_clients": 2,
    "packets_processed": 15420,
    "uptime_seconds": 3600,
    "memory_usage_mb": 45.2,
    "connection_type": "socket"
  },
  "error": null,
  "timestamp": 1704067200000
}
```

### Device Control

#### Start Data Collection
```http
POST /api/control/start
```

**Response:**
```json
{
  "success": true,
  "data": "Data collection started",
  "error": null,
  "timestamp": 1704067200000
}
```

#### Stop Data Collection
```http
POST /api/control/stop
```

**Response:**
```json
{
  "success": true,
  "data": "Data collection stopped",
  "error": null,
  "timestamp": 1704067200000
}
```

#### Ping Device
```http
POST /api/control/ping
```

Tests device connectivity and responsiveness.

**Response:**
```json
{
  "success": true,
  "data": "Ping command sent to device",
  "error": null,
  "timestamp": 1704067200000
}
```

#### Get Device Information
```http
POST /api/control/device_info
```

Requests detailed device capabilities and information.

**Response:**
```json
{
  "success": true,
  "data": "Device info request sent",
  "error": null,
  "timestamp": 1704067200000
}
```

### Device Mode Control

#### Set Continuous Mode
```http
POST /api/control/continuous_mode
```

Configures the device for continuous data streaming.

#### Set Trigger Mode
```http
POST /api/control/trigger_mode
```

Configures the device for event-triggered data collection.

#### Configure Data Stream
```http
POST /api/control/configure

Content-Type: application/json
```

**Request Body:**
```json
{
  "channels": [
    {
      "channel_id": 0,
      "sample_rate": 10000,
      "format": 1
    },
    {
      "channel_id": 1,
      "sample_rate": 10000,
      "format": 1
    }
  ]
}
```

**Format Values:**
- `1`: int16
- `2`: int32  
- `4`: float32

### File Management

#### List Files
```http
GET /api/files
GET /api/files?dir=subfolder
```

**Response:**
```json
{
  "success": true,
  "data": [
    {
      "filename": "wave_20240101_143022.bin",
      "size_bytes": 2048000,
      "created_at": 1704110422000,
      "file_type": "binary"
    }
  ],
  "error": null,
  "timestamp": 1704067200000
}
```

#### Download File
```http
GET /api/files/{filename}
```

Downloads the specified file. Supports subdirectory paths like `subfolder/file.bin`.

**Response:** Binary file content with appropriate headers.

#### Save Data File
```http
POST /api/files/save

Content-Type: application/json
```

**Request Body:**
```json
{
  "dir": "measurements/2024-01-01",
  "filename": "test_data.bin",
  "base64": "AAABAAACAAADAAAEAAAF..."
}
```

**Parameters:**
- `dir` (optional): Subdirectory path relative to data directory
- `filename` (optional): Custom filename, auto-generated if not provided
- `base64` (required): Base64-encoded file content

**Response:**
```json
{
  "success": true,
  "data": "measurements/2024-01-01/test_data.bin",
  "error": null,
  "timestamp": 1704067200000
}
```

### Health Check

#### System Health
```http
GET /health
```

**Response:**
```json
{
  "success": true,
  "data": {
    "status": "healthy",
    "service": "data-processor",
    "version": "2.0",
    "timestamp": "2024-01-01T12:00:00Z"
  },
  "error": null,
  "timestamp": 1704067200000
}
```

### API Information
```http
GET /
```

Returns API documentation and available endpoints.

## WebSocket Interface

### Connection
Connect to: `ws://{host}:{port}`

### Data Messages

Real-time processed data is streamed as JSON:

```json
{
  "type": "data",
  "timestamp": 1704067200000,
  "sequence": 12345,
  "channel_count": 2,
  "sample_rate": 10000.0,
  "data": [1.23, 1.24, 1.25, 1.26, ...],
  "metadata": {
    "packet_count": 150,
    "processing_time_us": 120,
    "data_quality": {
      "status": "Good"
    }
  }
}
```

### Welcome Message
Upon connection, clients receive:

```json
{
  "type": "welcome",
  "client_id": "550e8400-e29b-41d4-a716-446655440000",
  "timestamp": 1704067200000
}
```

### Error Messages
System errors are broadcasted:

```json
{
  "type": "error",
  "error_code": "DEVICE_DISCONNECTED",
  "message": "Device connection lost",
  "timestamp": 1704067200000
}
```

## Error Handling

### HTTP Status Codes
- `200 OK`: Successful operation
- `400 Bad Request`: Invalid request parameters
- `404 Not Found`: File or resource not found
- `500 Internal Server Error`: System error

### Error Response Format
```json
{
  "success": false,
  "data": null,
  "error": "Detailed error message",
  "timestamp": 1704067200000
}
```

### Common Error Scenarios

#### Device Connection Issues
- **Symptom**: API calls return errors, no data streaming
- **Solution**: Check device connection, verify configuration
- **Endpoints**: Use `/api/control/ping` to test connectivity

#### File Access Errors
- **Symptom**: File operations fail with 500 errors
- **Solution**: Check data directory permissions and disk space
- **Prevention**: Monitor disk usage and configure appropriate limits

#### WebSocket Connection Issues
- **Symptom**: Clients can't connect or receive data
- **Solution**: Verify WebSocket port and firewall settings
- **Monitoring**: Check connected client count in status endpoint

## Configuration Reference

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `DEVICE_TYPE` | socket | Connection type: "serial" or "socket" |
| `SERIAL_PORT` | COM7 | Serial port for USB-CDC connection |
| `SOCKET_ADDRESS` | 127.0.0.1:9001 | TCP socket address for testing |
| `BAUD_RATE` | 115200 | Serial communication baud rate |
| `WEB_HOST` | 127.0.0.1 | HTTP server bind address |
| `WEB_PORT` | 8080 | HTTP server port |
| `WS_HOST` | 127.0.0.1 | WebSocket server bind address |
| `WS_PORT` | 8081 | WebSocket server port |
| `DATA_DIR` | ./data | Data storage directory |
| `FILE_PREFIX` | wave | Auto-generated filename prefix |
| `FILE_EXT` | .bin | Auto-generated file extension |
| `MAX_FILES` | 200 | Maximum files in data directory |

### Data Processing Settings

The system applies the following processing pipeline:
1. **Protocol Parsing**: Validates frames and extracts data
2. **Format Conversion**: Converts raw ADC values to engineering units
3. **Filtering**: Applies 5-point moving average smoothing
4. **Quality Assessment**: Evaluates data quality and flags anomalies

## Architecture

### Core Components

```
data-processor/
├── src/
│   ├── main.rs                    # Application entry point
│   ├── config.rs                  # Configuration management
│   ├── device_communication.rs    # Device protocol implementation
│   ├── data_processing.rs         # Real-time data processing
│   ├── web_server.rs             # REST API server
│   ├── websocket.rs              # WebSocket streaming
│   └── file_manager.rs           # File storage management
├── Cargo.toml                    # Rust dependencies
└── .env                         # Environment configuration
```

### Data Flow

```
Device → Protocol Parser → Data Processor → Quality Assessor → {WebSocket Broadcast, File Storage}
                                         ↑
                           REST API ← Web Server
```

### Concurrency Model

- **Async/Await**: Built on Tokio async runtime
- **Message Passing**: Uses channels for inter-task communication
- **Shared State**: Minimal shared state with appropriate synchronization
- **Backpressure**: Handles slow consumers gracefully

## Deployment

### Docker Deployment
```bash
# Build container
docker build -t data-processor .

# Run with environment variables
docker run -d \
  -p 8080:8080 \
  -p 8081:8081 \
  -v $(pwd)/data:/app/data \
  -e DEVICE_TYPE=socket \
  -e SOCKET_ADDRESS=host.docker.internal:9001 \
  data-processor
```

### Production Considerations

#### Security
- Run with minimal privileges
- Use firewall to restrict WebSocket access
- Validate all file paths to prevent directory traversal
- Monitor API access and rate limiting

#### Performance
- Allocate sufficient memory for data buffers
- Monitor CPU usage during high-throughput operations  
- Use SSD storage for data directory
- Consider data retention policies

#### Monitoring
- Monitor system status endpoint regularly
- Set up alerts for device disconnections
- Track memory and disk usage
- Log analysis for error patterns

## Troubleshooting

### Device Connection Issues

**Problem**: Device not connecting
```bash
# Check device availability
# For serial: verify port exists and permissions
ls -la /dev/tty* # Linux
# For socket: test connectivity
telnet 127.0.0.1 9001
```

**Solution**: 
- Verify device configuration
- Check cable connections
- Confirm device simulator is running

### High Memory Usage

**Problem**: Memory consumption growing over time
- Monitor connected WebSocket clients
- Check data directory size
- Review buffer configurations

**Solution**:
- Disconnect unused WebSocket clients
- Increase MAX_FILES cleanup threshold
- Restart service if memory leak suspected

### Data Quality Issues

**Problem**: Poor data quality warnings
- Check device signal integrity
- Verify sampling rates are appropriate
- Monitor for electrical interference

**Solution**:
- Adjust trigger thresholds
- Use better shielded cables
- Ground device properly

## Development

### Building from Source
```bash
# Install Rust toolchain
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Clone and build
git clone <repository>
cd data-processor
cargo build --release

# Run tests
cargo test

# Check code style
cargo clippy
cargo fmt
```

### Adding Features

1. **New API Endpoints**: Modify `web_server.rs`
2. **Device Commands**: Extend `device_communication.rs` 
3. **Data Processing**: Update `data_processing.rs`
4. **Configuration**: Add to `config.rs` and environment variables

### Protocol Extension

To add new device commands:
1. Define command ID in `device_communication.rs`
2. Implement request/response handling
3. Add corresponding API endpoint if needed
4. Update protocol documentation

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Support

For issues and feature requests, please use the project's issue tracker.