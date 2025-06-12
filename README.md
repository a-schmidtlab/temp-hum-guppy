# ESP32 Temperature & Humidity Logger

A temperature and humidity monitoring solution using ESP32-Eth01 and DHT11 sensor with a web dashboard.

![ESP32 Temperature Monitor](https://img.shields.io/badge/ESP32-Temperature%20Monitor-blue)
![Status](https://img.shields.io/badge/Status-Working-green)

##  Features

- **Real-time monitoring** of temperature and humidity (30-second intervals)
- **Beautiful web dashboard** with interactive charts and auto-scaling
- **Smart data retention**: Detailed (30min) + Aggregated (24h) + Combined views
- **🚨 Enterprise-style alert system** with Star Trek bridge sounds and visual effects
- **Temperature threshold alerts** (configurable, default: 40°C)
- **Automatic network switching**: WiFi ↔ Ethernet with easy discovery
- **Auto-refresh**: Updates every 30 seconds with network status indicators
- **Data logging**: Intelligent buffering with detailed and aggregated storage
- **Responsive design**: Works perfectly on mobile and desktop
- **Network discovery**: Access via hostname (tr-cam1-t-h-sensor.local)

## Hardware Requirements (Bill of Materials)

| Qty | Component | Description | Notes |
|-----|-----------|-------------|-------|
| 1 | **ESP32-Eth01 v1.4 (V1781)** | ESP32 with Ethernet PHY | Main microcontroller |
| 1 | **DHT11 Sensor** | Temperature/Humidity sensor | 3-5V, basic accuracy |
| 1 | **USB-TTL Adapter (HW-193)** | Serial programmer | 3.3V logic level |
| 1 | **5V Power Supply** | DC adapter or USB charger | Min 500mA recommended |
| - | **Dupont Jumper Wires** | Male-to-male, male-to-female | For connections |
| - | **Small Breadboard** | Optional | Makes wiring easier |

## Hardware Setup

###  **CRITICAL: Power Setup**

** NEVER connect more than 3.6V to the 3V3 pin - it's an OUTPUT only!**

```
Power Supply (5V) → ESP32-Eth01 5V pin → AMS1117 LDO → 3.3V output
```

1. **Connect Power:**
   - Power supply **positive** → ESP32-Eth01 **5V pin**
   - Power supply **negative** → ESP32-Eth01 **GND pin**

2. **Verify Voltage:**
   - Measure voltage at **3V3 pin** with multimeter
   - Should read **3.25V - 3.35V**
   - If < 3.25V: Disable Ethernet in code (`USE_ETH = false`)

###  **DHT11 Sensor Wiring**

```
ESP32-Eth01          DHT11 Sensor
┌──────────────┐     ┌──────────────┐
│ 3V3 (out) ──┼─────┼── VCC (red)  │
│ GND ────────┼─────┼── GND (black)│
│ GPIO4 ──────┼─────┼── DATA (yellow)│
└──────────────┘     └──────────────┘
```

**Connections:**
- DHT11 **VCC** (red) → ESP32-Eth01 **3V3 pin**
- DHT11 **GND** (black) → ESP32-Eth01 **GND pin**
- DHT11 **DATA** (yellow) → ESP32-Eth01 **GPIO4 pin**

###  **USB-TTL Programming Setup**

```
USB-TTL (HW-193)     ESP32-Eth01
┌──────────────┐     ┌──────────────┐
│ TX ──────────┼─────┼── U0RXD      │
│ RX ──────────┼─────┼── U0TXD      │
│ GND ─────────┼─────┼── GND        │
│ VCC (don't connect) │              │
└──────────────┘     └──────────────┘
```

**Programming Connections:**
- USB-TTL **TX** → ESP32-Eth01 **U0RXD**
- USB-TTL **RX** → ESP32-Eth01 **U0TXD**
- USB-TTL **GND** → ESP32-Eth01 **GND**
- **Do NOT connect USB-TTL VCC** (ESP32 already powered)

###  **Programming Mode (For Firmware Upload)**

**To flash firmware:**

1. **Enter Programming Mode:**
   - Connect **GPIO0** to **GND** with jumper wire
   - Reset ESP32 (briefly connect **EN** to **GND** or cycle power)
   - **Keep GPIO0 connected to GND** during entire flash process

2. **After Flashing:**
   - **Disconnect GPIO0 from GND**
   - Reset ESP32 (briefly connect **EN** to **GND**)
   - ESP32 will boot in normal mode

##  Software Setup

###  **Prerequisites (Linux/Ubuntu/Mint)**

```bash
# Install basic tools
sudo apt update && sudo apt install python3 python3-venv git

# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install PlatformIO
pip install platformio
```

###  **Project Setup**

```bash
# Clone or create project directory
mkdir esp32-temp-logger
cd esp32-temp-logger

# Initialize PlatformIO project
pio project init --board wt32-eth01

# The main firmware is in src/main.cpp
# Configuration is in platformio.ini
```

###  **Configuration**

**Edit `src/main.cpp`** and update WiFi credentials:

```cpp
const char *SSID = "YOUR_WIFI_NAME";    // 2.4GHz WiFi only!
const char *PASS = "YOUR_WIFI_PASSWORD";
```

**Key settings in code:**
- `USE_ETH = true` - Enable Ethernet (set false if 3V3 < 3.25V)
- `DHTPIN = 4` - GPIO pin for DHT11 data
- `SAMPLE_MS = 300000UL` - Reading interval (5 minutes)

##  **Building & Flashing**

###  **Compile Firmware**

```bash
source venv/bin/activate
pio run
```

###  **Upload Firmware**

#### **Method 1: Simple Upload with Manual Reset (Recommended - Most Reliable)**

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

4. **Manual reset sequence (timing is everything):**
   - **Quickly connect GPIO0 to GND** with jumper wire
   - **Immediately reset ESP32** (briefly touch EN pin to GND or power cycle)
   - **Keep GPIO0 connected to GND** throughout the entire upload process

5. **Success indicators (you'll see these if timing was correct):**
   - Terminal shows: "Chip is ESP32-D0WD-V3 (revision v3.1)"
   - **Hex addresses with percentages**: 
     ```
     Writing at 0x00010000... (2 %)
     Writing at 0x0001d8f3... (5 %)
     Writing at 0x0002a56e... (7 %)
     ...
     Writing at 0x000fa724... (100 %)
     ```
   - Upload completes: "**[SUCCESS] Took X seconds**"

6. **Exit programming mode:**
   - **Disconnect GPIO0 from GND**
   - **Reset ESP32** (briefly touch EN to GND)
   - ESP32 boots in normal mode

**💡 Pro Tips:**
- **Perfect timing is crucial** - reset exactly when the dots start appearing
- **If upload fails**, just try again - the timing takes practice
- **Successful uploads show hex addresses** - if you don't see them, timing was off

#### **Method 2: Standard Upload (If Method 1 Fails)**

```bash
# Alternative approach - put in programming mode first
# 1. Connect GPIO0 to GND
# 2. Reset ESP32
# 3. Run upload command
source venv/bin/activate
pio run -t upload

# 4. When upload completes, disconnect GPIO0 from GND
# 5. Reset ESP32
```

#### **Upload Configuration (platformio.ini)**

The project includes optimized upload settings:

```ini
upload_speed = 115200          # Reliable speed for ESP32-ETH01
upload_resetmethod = nodemcu   # Compatible reset method
upload_flags = 
    --before=default_reset     # Reset before upload
    --after=hard_reset         # Hard reset after upload
    --connect-attempts=30      # More connection attempts
```

#### **Troubleshooting Upload Issues**

| Error | Cause | Solution |
|-------|-------|----------|
| `Could not open /dev/ttyUSB0` | USB-TTL not detected | Check USB connection, install drivers |
| `Failed to connect to ESP32` | Not in programming mode | Use Method 1 with manual reset timing |
| `No serial data received` | Wrong timing or wiring | Check GPIO0→GND during "Connecting..." |
| Upload starts but fails | Power issues | Ensure adequate 5V power supply (≥1A) |

**⚠️ Critical Notes:**
- **Timing is crucial** - reset exactly when "Connecting..." dots appear
- **Keep GPIO0 grounded** until you see hex addresses and percentages
- **Never connect USB-TTL VCC** to ESP32 (ESP32 separately powered)
- **Use adequate power supply** - insufficient current causes upload failures

### **Monitor Serial Output**

```bash
source venv/bin/activate
pio device monitor --baud 115200
```

**Expected output:**
```
ESP32 Temperature/Humidity Logger Starting...
DHT11 sensor initialized on GPIO4
Initializing Ethernet...
No Ethernet cable detected, using WiFi
Connecting to WiFi...
....
WiFi connected!
IP address: 192.168.x.x
Web server started
Reading: 27.2°C, 31% RH
Setup complete!
```

## **Usage**

### **Access Dashboard**

1. **Note the IP address** from serial output
2. **Open web browser** and navigate to: `http://192.168.x.x`
3. **Enjoy your dashboard!**

### **Dashboard Features**

- **Real-time current readings** displayed prominently
- **Time range selector**: Detailed (30s intervals, 30min), Aggregated (5min intervals, 24h), Combined (all data)
- **Interactive charts** for temperature and humidity with auto-scaling
- **Auto-refresh** every 30 seconds for current data
- **Responsive design** works on mobile and desktop

#### **🚨 Enterprise-Style Alert System**

- **Temperature threshold alerts** (default: 40°C, user-configurable)
- **Star Trek Enterprise bridge-style alert sound** using Web Audio API
- **Visual red alert animation** with flashing background
- **Alert status indicators**:
  - ✅ Normal: Temperature within limits
  - 🚨 **RED ALERT**: Temperature critical (with sound and animation)
  - ⚠️ Alert acknowledged: Temperature still elevated but user notified
- **One-click alert acknowledgment** to stop sound
- **Alert text**: "🚨 RED ALERT! TEMPERATURE CRITICAL!"
- **Console logging** for debugging alert system

### **Network Switching**

- **WiFi Primary**: Connects to 2.4GHz WiFi
- **Ethernet Auto-Switch**: Plug in cable anytime → auto-switches
- **Fallback**: If Ethernet unplugged → switches back to WiFi

### **Easy Network Discovery**

**Your ESP32 is now super easy to find on any network!**

#### **Quick Access (Recommended)**
- **Just open your browser** and go to: `http://tr-cam1-t-h-sensor.local`
- **Works on most networks** without knowing the IP address
- **Bookmark it** for instant access

#### **LED Status Indicators**
Watch the built-in LED for connection status:
- **2 slow blinks**: Trying to connect
- **3 quick blinks**: Successfully connected  
- **1 long blink**: Network disconnected
- **1 quick blink every 5 min**: Taking sensor reading

#### **For Foreign Networks (Hotels, Offices, etc.)**
See detailed guide: **[NETWORK_DISCOVERY.md](NETWORK_DISCOVERY.md)**

**Multiple ways to find your device:**
1. **Browser**: `http://tr-cam1-t-h-sensor.local` (easiest)
2. **Router admin**: Look for "tr-cam1-t-h-sensor" in device list
3. **Mobile apps**: Use "Fing" network scanner
4. **Serial monitor**: Check terminal output for IP address

## **Troubleshooting**

### **Common Issues**

| Problem | Solution |
|---------|----------|
| **No serial output** | Check TX/RX wiring, ensure GPIO0 disconnected |
| **Can't flash firmware** | Put in programming mode (GPIO0→GND, reset) |
| **WiFi won't connect** | Use 2.4GHz network, check credentials |
| **Ethernet not working** | Check 3V3 voltage ≥3.25V, set `USE_ETH=false` if needed |
| **No sensor readings** | Verify DHT11 wiring to GPIO4 |
| **Web page won't load** | Check IP address in serial monitor |

### **Voltage Troubleshooting**

- **3V3 pin measures < 3.25V**: Set `USE_ETH = false` in code
- **3V3 pin measures > 3.35V**: Check power supply, use different PSU
- **Blue LED dim**: Insufficient power supply current

### **Hardware Double-Check**

1. **Power**: 5V → ESP32 5V pin (NOT 3V3 pin)
2. **DHT11**: VCC→3V3, GND→GND, DATA→GPIO4
3. **USB-TTL**: TX→U0RXD, RX→U0TXD, GND→GND
4. **Programming**: GPIO0→GND only during flash

## **Technical Details**

### **Specifications**

- **Microcontroller**: ESP32-D0WD-V3
- **RAM Usage**: ~44KB (13.4%)
- **Flash Usage**: ~866KB (66%)
- **Sample Rate**: Every 5 minutes
- **Data Retention**: 24h (RAM), 7d+30d (persistent)
- **Network**: WiFi 2.4GHz + Ethernet 10/100
- **Web Server**: AsyncWebServer with REST API

### **API Endpoints**

- `GET /` - Main dashboard with Enterprise alert system
- `GET /api/current` - Current temperature/humidity JSON with sampling info
- `GET /api/history?range=detailed|aggregated|all` - Historical data JSON
- `GET /api/alert/get` - Current alert status and threshold
- `POST /api/alert/set` - Set temperature alert threshold (°C)
- `POST /api/alert/acknowledge` - Acknowledge active temperature alert

### **Data Storage**

- **Detailed buffer**: 30 minutes of 30-second samples in RAM (60 samples)
- **Aggregated buffer**: ~24 hours of 5-minute averages in RAM (288 samples)
- **Smart aggregation**: Automatically converts old detailed data to 5-minute averages
- **Memory efficient**: Uses std::deque and std::vector for optimal performance
- **Real-time processing**: No flash wear from constant writing

*END*



## **License**

This project is open source. Feel free to modify and share!


(c) 2025 by Axel Schmidt