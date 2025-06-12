# ESP32 Temperature & Humidity Logger

A professional temperature and humidity monitoring solution using ESP32-ETH01 and DHT11 sensor with persistent data storage, intelligent memory management, and enterprise-grade alert system.

![ESP32 Temperature Monitor](https://img.shields.io/badge/ESP32-Temperature%20Monitor-blue)
![Status](https://img.shields.io/badge/Status-Production%20Ready-green)
![Data Storage](https://img.shields.io/badge/Data-Persistent%20Storage-orange)
![Memory Safe](https://img.shields.io/badge/Memory-Overflow%20Protected-red)

## Key Features

### Real-Time Monitoring
- **30-second measurement intervals** with DHT11 sensor
- **Beautiful web dashboard** with interactive auto-scaling charts
- **Smart data retention**: Detailed (30min) + Aggregated (24h) + Historical (7d)
- **Real-time system status**: Memory usage, storage health, operation mode
- **Responsive design**: Perfect on mobile and desktop

### Persistent Data Storage (Power-Safe)
- **SPIFFS flash storage** - Data survives power outages and reboots
- **Automatic hourly saves** - No data loss during unexpected shutdowns  
- **Auto-load on startup** - Previous data restored when ESP32 restarts
- **Configuration persistence** - Alert settings saved immediately
- **7-day data retention** - Keep historical data through power cycles

### Memory Management & Overflow Protection
- **Real-time memory monitoring** - Tracks RAM usage every 30 seconds
- **Smart emergency mode** - Automatic data compression at 80% memory usage
- **Critical protection** - Emergency cleanup at 90% memory usage
- **Intelligent aggregation** - Converts old detailed data to space-efficient averages
- **Self-healing system** - Never runs out of memory, even with months of uptime

### Enterprise-Grade Alert System
- **Multi-method audio alerts** with browser compatibility fallbacks:
  - **Web Audio API beeps** (loud, clear square waves)
  - **Text-to-speech announcements** ("Red Alert! Temperature Critical!")
  - **System audio fallback** for maximum compatibility
- **Temperature threshold alerts** (configurable, default: 40°C)
- **Humidity threshold alerts** (configurable, default: 90%)
- **Visual status indicators** with green "Audio Ready" confirmation
- **One-click acknowledgment** to stop alerts
- **User interaction required** - Modern browser security compliance
- **Continuous alerting** - Alerts persist until manually acknowledged

### Advanced Networking
- **Automatic network switching**: WiFi ↔ Ethernet with seamless fallback
- **Easy discovery**: Access via `http://tr-cam1-t-h-sensor.local`
- **Network status indicators**: LED patterns show connection status
- **mDNS service discovery** for easy device finding
- **Foreign network support**: Works in hotels, offices, any network

## Open Issues

### Alarm Sound Behavior
- **Issue**: The alarm sound currently only plays once per measurement cycle instead of continuously until acknowledged
- **Current Status**: The system is designed to play continuous alerts every second until acknowledged, but there may be interference from multiple timer calls
- **Workaround**: The alert system is functional but may need optimization for continuous audio playback
- **Priority**: Medium - System is usable but audio experience could be improved

### Known Limitations
- **DHT11 Sensor**: Limited to 30-second minimum reading intervals due to sensor specifications
- **Browser Audio**: Requires user interaction (click "TEST AUDIO") before alerts can play due to browser security policies
- **Memory Usage**: System automatically manages memory but may enter emergency mode during extended operation

## Hardware Requirements

| Qty | Component | Description | Notes |
|-----|-----------|-------------|-------|
| 1 | **ESP32-ETH01 v1.4 (V1781)** | ESP32 with Ethernet PHY | Main microcontroller |
| 1 | **DHT11 Sensor** | Temperature/Humidity sensor | 3-5V, ±2°C accuracy |
| 1 | **USB-TTL Adapter (HW-193)** | Serial programmer | 3.3V logic level |
| 1 | **5V Power Supply** | DC adapter or USB charger | Min 1A recommended |
| - | **Dupont Jumper Wires** | Male-to-male, male-to-female | For connections |
| - | **Small Breadboard** | Optional | Makes wiring easier |

## Hardware Setup

### CRITICAL: Power Setup

**NEVER connect more than 3.6V to the 3V3 pin - it's an OUTPUT only!**

```
Power Supply (5V) → ESP32-ETH01 5V pin → AMS1117 LDO → 3.3V output
```

1. **Connect Power:**
   - Power supply **positive** → ESP32-ETH01 **5V pin**
   - Power supply **negative** → ESP32-ETH01 **GND pin**

2. **Verify Voltage:**
   - Measure voltage at **3V3 pin** with multimeter
   - Should read **3.25V - 3.35V**
   - If < 3.25V: Disable Ethernet in code (`USE_ETH = false`)

### DHT11 Sensor Wiring

```
ESP32-ETH01          DHT11 Sensor
┌──────────────┐     ┌──────────────┐
│ 3V3 (out) ──┼─────┼── VCC (red)  │
│ GND ────────┼─────┼── GND (black)│
│ GPIO4 ──────┼─────┼── DATA (yellow)│
└──────────────┘     └──────────────┘
```

**Connections:**
- DHT11 **VCC** (red) → ESP32-ETH01 **3V3 pin**
- DHT11 **GND** (black) → ESP32-ETH01 **GND pin**  
- DHT11 **DATA** (yellow) → ESP32-ETH01 **GPIO4 pin**

### USB-TTL Programming Setup

```
USB-TTL (HW-193)     ESP32-ETH01
┌──────────────┐     ┌──────────────┐
│ TX ──────────┼─────┼── U0RXD      │
│ RX ──────────┼─────┼── U0TXD      │
│ GND ─────────┼─────┼── GND        │
│ VCC (don't connect) │              │
└──────────────┘     └──────────────┘
```

**Programming Connections:**
- USB-TTL **TX** → ESP32-ETH01 **U0RXD**
- USB-TTL **RX** → ESP32-ETH01 **U0TXD**
- USB-TTL **GND** → ESP32-ETH01 **GND**
- **Do NOT connect USB-TTL VCC** (ESP32 already powered)

### Programming Mode (For Firmware Upload)

**To flash firmware:**

1. **Enter Programming Mode:**
   - Connect **GPIO0** to **GND** with jumper wire
   - Reset ESP32 (briefly connect **EN** to **GND** or cycle power)
   - **Keep GPIO0 connected to GND** during entire flash process

2. **After Flashing:**
   - **Disconnect GPIO0 from GND**
   - Reset ESP32 (briefly connect **EN** to **GND**)
   - ESP32 will boot in normal mode

## Software Setup

### Prerequisites (Linux/Ubuntu/Mint)

```bash
# Install basic tools
sudo apt update && sudo apt install python3 python3-venv git

# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install PlatformIO
pip install platformio
```

### Project Setup

```bash
# Clone or create project directory
mkdir esp32-temp-logger
cd esp32-temp-logger

# Initialize PlatformIO project
pio project init --board wt32-eth01

# The main firmware is in src/main.cpp
# Configuration is in platformio.ini
```

### Configuration

**Edit `src/main.cpp`** and update WiFi credentials:

```cpp
const char *SSID = "YOUR_WIFI_NAME";    // 2.4GHz WiFi only!
const char *PASS = "YOUR_WIFI_PASSWORD";
```

**Key settings in code:**
- `USE_ETH = true` - Enable Ethernet (set false if 3V3 < 3.25V)
- `DHTPIN = 4` - GPIO pin for DHT11 data
- `SAMPLE_MS = 30000UL` - Reading interval (30 seconds)
- `alertThreshold = 40.0` - Default temperature alert (°C)

## Building & Flashing

### Compile Firmware

```bash
source venv/bin/activate
pio run
```

**Expected build results:**
```
RAM:   [=         ]  14.8% (used 48KB from 320KB)
Flash: [=======   ]  74.9% (used 981KB from 1.3MB)
```

### Upload Firmware

#### Method 1: Manual Reset Upload (Most Reliable)

This method works reliably when the ESP32-ETH01 board lacks a physical BOOT button:

1. **Prepare hardware:**
   - Connect USB-TTL adapter to ESP32 (TX→U0RXD, RX→U0TXD, GND→GND)
   - Ensure ESP32 is powered (5V supply connected)
   - Have a jumper wire ready for GPIO0→GND connection

2. **Start upload process:**
   ```bash
   source venv/bin/activate
   pio run --target upload
   ```

3. **Watch for connection phase:**
   - Monitor terminal output for "Connecting..." with dots appearing
   - **Critical timing**: As soon as you see "Connecting..................", perform the manual reset

4. **Manual reset sequence (timing is crucial):**
   - **Quickly connect GPIO0 to GND** with jumper wire
   - **Immediately reset ESP32** (briefly touch EN pin to GND or power cycle)
   - **Keep GPIO0 connected to GND** throughout the entire upload process

5. **Success indicators:**
   - Terminal shows: "Chip is ESP32-D0WD-V3 (revision v3.1)"
   - **Hex addresses with percentages**: 
     ```
     Writing at 0x00010000... (2 %)
     Writing at 0x0001df46... (5 %)
     Writing at 0x0002a887... (7 %)
     ...
     Writing at 0x000fc508... (100 %)
     ```
   - Upload completes: "**[SUCCESS] Took X seconds**"

6. **Exit programming mode:**
   - **Disconnect GPIO0 from GND**
   - **Reset ESP32** (briefly touch EN to GND)
   - ESP32 boots in normal mode

### Monitor Serial Output

```bash
source venv/bin/activate
pio device monitor --baud 115200
```

**Expected startup output:**
```
ESP32 Temperature/Humidity Logger Starting...
DHT11 sensor initialized on GPIO4
SPIFFS initialized - persistent storage ready
Loading data from persistent storage...
Loaded 245 historical records from persistent storage
Memory usage at startup: 18% (260 KB free)
Initializing Ethernet...
WiFi connected!
NTP time synchronized!
Web server started
Reading DHT sensor...
Reading [2024-01-15 14:30:15]: 23.2°C, 45% RH (detailed: 1 samples)
Memory: 19% used (258 KB free), Buffers: 1 detailed + 245 aggregated
Setup complete!
```

## Usage

### Access Dashboard

**Multiple ways to access your sensor:**

1. **Easiest: Magic Address**
   - Open browser: `http://tr-cam1-t-h-sensor.local`
   - Works on most modern networks

2. **Direct IP Access**
   - Note IP address from serial output
   - Open browser: `http://192.168.x.x`

3. **Network Discovery**
   - Use "Fing" app on mobile
   - Look for "tr-cam1-t-h-sensor" device

### Dashboard Features

#### Audio Alert System
1. **MUST CLICK "TEST SOUND" FIRST** - Required for browser audio permission
2. **Green "Audio Ready" status** appears after successful test
3. **Automatic alerts** when temperature or humidity exceeds thresholds
4. **Multiple fallback methods**: Web Audio → Text-to-Speech → System Audio
5. **One-click acknowledgment** to stop alerts
6. **Dual monitoring**: Separate thresholds for temperature and humidity

#### Data Visualization
- **Real-time current readings** with timestamp and system status
- **Time range selector**: 
  - **Detailed**: 30-second intervals, last 30 minutes
  - **Aggregated**: 5-minute intervals, ~24 hours  
  - **Combined**: All available data (detailed + aggregated)
- **Interactive charts** for temperature and humidity with auto-scaling
- **Auto-refresh** every 30 seconds for current data

#### System Status Indicators
- **Memory Usage**: Real-time RAM monitoring (Green: <80%, Orange: 80-90%, Red: >90%)
- **Storage Status**: Active (SPIFFS working) or Failed
- **Operation Mode**: Normal or Emergency (memory protection active)

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Main dashboard with audio alert system |
| `/api/current` | GET | Current temperature/humidity + system status |
| `/api/history?range=detailed\|aggregated\|all` | GET | Historical data |
| `/api/alert/get` | GET | Current temperature alert status and threshold |
| `/api/alert/set` | POST | Set temperature alert threshold (°C) |
| `/api/alert/acknowledge` | POST | Acknowledge active temperature alert |
| `/api/humidity-alert/get` | GET | Current humidity alert status and threshold |
| `/api/humidity-alert/set` | POST | Set humidity alert threshold (%) |
| `/api/humidity-alert/acknowledge` | POST | Acknowledge active humidity alert |
| `/api/save` | POST | Force save data to persistent storage |

### Data Storage System

#### RAM Storage (Fast Access)
- **Detailed Buffer**: 30 minutes of 30-second samples (60 samples max)
- **Aggregated Buffer**: ~24 hours of 5-minute averages (288 samples max)
- **Automatic cleanup**: Old detailed data converted to aggregated

#### Flash Storage (Persistent)
- **Historical Data**: Up to 7 days of 5-minute averages (2016 records)
- **Configuration**: Alert thresholds and settings
- **Auto-save**: Every hour + immediate config saves
- **Power-safe**: Survives reboots, power outages, crashes

#### Memory Protection
- **80% RAM usage**: Emergency aggregation mode (faster data compression)
- **90% RAM usage**: Critical cleanup (aggressive data reduction)
- **Self-healing**: System never runs out of memory

## Troubleshooting

### Common Issues

| Problem | Solution |
|---------|----------|
| **No serial output** | Check TX/RX wiring, ensure GPIO0 disconnected |
| **Can't flash firmware** | Use Method 1 manual reset timing (GPIO0→GND during "Connecting...") |
| **WiFi won't connect** | Use 2.4GHz network, check credentials |
| **Ethernet not working** | Check 3V3 voltage ≥3.25V, set `USE_ETH=false` if needed |
| **No sensor readings** | Verify DHT11 wiring to GPIO4, check 3V3 power |
| **Web page won't load** | Check IP address in serial monitor |
| **Audio alerts don't work** | Click "TEST SOUND" button first to enable browser audio |
| **Memory issues** | Check system status - emergency mode activates automatically |
| **Lost data after power outage** | Check serial log for "Loaded X historical records" |

### LED Status Indicators

| LED Pattern | Meaning |
|-------------|---------|
| 2 slow blinks | Trying to connect to network |
| 3 quick blinks | Successfully connected |
| 1 long blink | Network disconnected |
| 1 quick blink every 30s | Taking sensor reading |

### Memory Monitoring

**Watch serial output for memory status:**
```
Memory: 25% used (240 KB free), Buffers: 60 detailed + 288 aggregated
HIGH MEMORY: 85% used - Starting emergency aggregation
CRITICAL MEMORY: 92% used - Emergency cleanup!
Memory normal: 45% used - Exiting emergency mode
```

### Data Recovery

**After power outage or restart:**
```
Loading data from persistent storage...
Loaded 1247 historical records from persistent storage
Loaded alert threshold: 35.5°C from persistent storage
```

## Technical Specifications

### Performance
- **Microcontroller**: ESP32-D0WD-V3 (240MHz dual-core)
- **RAM Usage**: ~48KB (14.8% of 320KB)
- **Flash Usage**: ~981KB (74.9% of 1.3MB)
- **Sample Rate**: Every 30 seconds
- **Network**: WiFi 2.4GHz + Ethernet 10/100Mbps

### Data Retention
- **RAM (Volatile)**: 30min detailed + 24h aggregated
- **Flash (Persistent)**: 7 days of 5-minute averages
- **Total Storage**: ~2400 data points through power cycles
- **Memory Protection**: Automatic cleanup prevents overflow

### Sensor Specifications
- **DHT11**: Temperature ±2°C, Humidity ±5% RH
- **Range**: -40°C to 80°C, 0-100% RH
- **Update Rate**: 30-second intervals (DHT11 limitation)

### Network Features
- **Discovery**: mDNS (.local domain), DHCP hostname
- **Protocols**: HTTP REST API, WebSocket-ready
- **Security**: Local network only, no external dependencies

## Production Readiness Features

**Data Persistence** - Survives power outages  
**Memory Management** - Prevents overflow crashes  
**Self-Healing** - Automatic error recovery  
**Network Resilience** - WiFi/Ethernet fallback  
**Real-Time Monitoring** - System health indicators  
**Enterprise Alerts** - Multi-method audio notifications  
**Easy Discovery** - Works on any network  
**Long-Term Reliability** - Months of continuous operation  

## Future Enhancements

- [ ] **InfluxDB integration** for long-term data storage
- [ ] **Grafana dashboard** for advanced visualization  
- [ ] **Email/SMS alerts** via SMTP/Twilio
- [ ] **Multiple sensor support** (DHT22, BME280)
- [ ] **Data export** to CSV/JSON
- [ ] **OTA firmware updates** via web interface

## License

This project is open source. Feel free to modify and share!

---

**Ready for Production Use**  
*Enterprise-grade temperature monitoring with persistent storage and intelligent memory management.*

(c) 2025 by Axel Schmidt