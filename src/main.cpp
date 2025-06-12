#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <DHT.h>
#include <ESPmDNS.h>        // Added for network discovery
#include <time.h>           // For NTP time synchronization
#include <deque>
#include <vector>
#include <map>

// ---------- CONFIG ----------
constexpr bool USE_ETH   = true;     // set false if 3V3 < 3.25 V
const char *SSID    = "RUT_F5DA_2G";
const char *PASS    = "i1V5FvDp";
const char *HOSTNAME = "tr-cam1-t-h-sensor";    // Device hostname for easy discovery
constexpr int   DHTPIN   = 4;        // GPIO4 for DHT11 data pin
constexpr int   DHTTYPE  = DHT11;
constexpr int   LED_PIN  = 2;        // Built-in LED for status indication
constexpr uint32_t SAMPLE_MS = 30000UL;    // 30-second measurement interval (DHT11 needs time)
constexpr uint32_t NETWORK_CHECK_MS = 30000UL;  // Check network every 30 seconds

// Data retention configuration
constexpr uint32_t DETAILED_PERIOD_SEC = 1800;   // Keep 30 minutes of detailed data
constexpr uint32_t AGGREGATE_INTERVAL_SEC = 300; // 5-minute aggregation for older data
constexpr uint32_t MAX_DETAILED_SAMPLES = DETAILED_PERIOD_SEC / (SAMPLE_MS / 1000); // 60 samples
constexpr uint32_t MAX_AGGREGATE_SAMPLES = 288;  // ~24 hours of 5-minute data

// NTP Time Configuration - Multiple sources for better reliability
const char* NTP_SERVERS[] = {
    "pool.ntp.org",           // Primary NTP server
    "time.google.com",        // Google time server
    "time.cloudflare.com",    // Cloudflare time server
    "time.nist.gov",          // NIST time server
    "192.168.1.1"             // Try local router (common gateway)
};
const int NTP_SERVER_COUNT = sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]);
const long  GMT_OFFSET_SEC = 3600;           // Germany: UTC+1 (3600 seconds)
const int   DAYLIGHT_OFFSET_SEC = 3600;      // Daylight saving time offset
// -----------------------------

struct Reading { 
  uint32_t ts;          // Unix timestamp (seconds since 1970)
  float t;              // Temperature in Celsius
  float h;              // Humidity in %
  String datetime;      // Human-readable date/time string
};

// Alert system variables
float alertThreshold = 40.0;     // Default alert temperature in Celsius
bool alertActive = false;        // Is an alert currently active?
bool alertAcknowledged = true;   // Has the current alert been acknowledged?
uint32_t lastAlertCheck = 0;

std::deque<Reading> detailedBuffer;     // 10 minutes of 10-second data
std::vector<Reading> aggregatedBuffer;   // Older data aggregated to 5-minute intervals

DHT dht(DHTPIN, DHTTYPE);
AsyncWebServer server(80);
uint32_t lastSample = 0;
uint32_t lastNetworkCheck = 0;
bool isConnected = false;

// Forward declarations
void aggregateOldData();
void setupNTP();
String getCurrentDateTime();
uint32_t getCurrentTimestamp();
void checkTemperatureAlert(float temperature);
void handleSetAlert(AsyncWebServerRequest *req);
void handleAckAlert(AsyncWebServerRequest *req);
void handleGetAlert(AsyncWebServerRequest *req);

// NTP Time Functions
void setupNTP() {
  Serial.println("Setting up NTP time synchronization...");
  
  // Try multiple NTP servers for better reliability
  bool timeSet = false;
  for (int attempt = 0; attempt < 3 && !timeSet; attempt++) {
    Serial.printf("NTP attempt %d/3\n", attempt + 1);
    
    // Configure NTP with multiple servers
    if (NTP_SERVER_COUNT >= 3) {
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, 
                 NTP_SERVERS[0], NTP_SERVERS[1], NTP_SERVERS[2]);
    } else if (NTP_SERVER_COUNT >= 2) {
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, 
                 NTP_SERVERS[0], NTP_SERVERS[1]);
    } else {
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVERS[0]);
    }
    
    // Wait for time to be set
    Serial.print("Waiting for NTP sync");
    int timeout = 0;
    while (!time(nullptr) && timeout < 15) {
      delay(1000);
      Serial.print(".");
      timeout++;
    }
    
    if (time(nullptr)) {
      timeSet = true;
      Serial.println("\n✅ NTP time synchronized!");
      Serial.printf("Current time: %s\n", getCurrentDateTime().c_str());
      Serial.printf("Timezone: UTC%+d (DST: %+d)\n", 
                   GMT_OFFSET_SEC/3600, DAYLIGHT_OFFSET_SEC/3600);
    } else {
      Serial.println("\n⚠️ NTP sync failed, trying next attempt...");
      delay(2000);
    }
  }
  
  if (!timeSet) {
    Serial.println("❌ All NTP attempts failed - using system millis() for timestamps");
    Serial.println("Time display will show relative time from boot");
  }
}

String getCurrentDateTime() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  
  char buffer[64];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

uint32_t getCurrentTimestamp() {
  time_t now;
  time(&now);
  return (uint32_t)now;
}

// Network discovery and status functions
void setupNetworkDiscovery() {
  // Set DHCP hostname for router device lists
  WiFi.setHostname(HOSTNAME);
  
  // Setup mDNS for .local domain access
  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("mDNS responder started: http://%s.local\n", HOSTNAME);
    
    // Add service advertisement
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "device", "temperature-sensor");
    MDNS.addServiceTxt("http", "tcp", "version", "1.0");
  } else {
    Serial.println("Error setting up mDNS responder!");
  }
}

void printNetworkInfo() {
  Serial.println("\n==================================================");
  Serial.println("NETWORK CONNECTION SUCCESS!");
  Serial.println("==================================================");
  
  if (ETH.linkUp()) {
    Serial.println("Connection Type: Ethernet");
    Serial.printf("IP Address: %s\n", ETH.localIP().toString().c_str());
    Serial.printf("Gateway: %s\n", ETH.gatewayIP().toString().c_str());
    Serial.printf("Subnet: %s\n", ETH.subnetMask().toString().c_str());
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connection Type: WiFi");
    Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("Subnet: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());
  }
  
  Serial.println("\nEASY ACCESS OPTIONS:");
  Serial.println("┌─────────────────────────────────────────┐");
  Serial.printf("│ 1. Browser:  http://%s.local       │\n", HOSTNAME);
  if (ETH.linkUp()) {
    Serial.printf("│ 2. Direct:   http://%-15s │\n", ETH.localIP().toString().c_str());
  } else {
    Serial.printf("│ 2. Direct:   http://%-15s │\n", WiFi.localIP().toString().c_str());
  }
  Serial.printf("│ 3. Hostname: %s                │\n", HOSTNAME);
  Serial.println("└─────────────────────────────────────────┘");
  Serial.println("\nTIP: Use option 1 on most networks!");
  Serial.println("==================================================\n");
}

void blinkStatusLED(int blinks, int delayMs = 200) {
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < blinks; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    delay(delayMs);
  }
}

void checkNetworkStatus() {
  bool currentlyConnected = false;
  
  if (USE_ETH && ETH.linkUp()) {
    currentlyConnected = true;
  } else if (WiFi.status() == WL_CONNECTED) {
    currentlyConnected = true;
  }
  
  if (currentlyConnected != isConnected) {
    isConnected = currentlyConnected;
    if (isConnected) {
      Serial.println("Network reconnected");
      blinkStatusLED(3, 100);  // 3 quick blinks = connected
      printNetworkInfo();
    } else {
      Serial.println("Network disconnected");
      blinkStatusLED(1, 1000); // 1 long blink = disconnected
    }
  }
}

// Helper functions
void addReading(float t, float h) {
  if (isnan(t) || isnan(h)) {
    Serial.printf("❌ DHT sensor read failed - T:%.2f, H:%.2f\n", t, h);
    return;
  }
  
  // Additional validation
  if (t < -40 || t > 80 || h < 0 || h > 100) {
    Serial.printf("❌ DHT sensor values out of range - T:%.2f, H:%.2f\n", t, h);
    return;
  }
  
  // Get current timestamp and formatted date/time
  uint32_t now = getCurrentTimestamp();
  String datetime = getCurrentDateTime();
  
  // If NTP failed, fall back to millis-based timestamp with boot offset
  if (now == 0 || now < 1000000000) {
    static uint32_t bootTime = millis() / 1000;
    now = bootTime + (millis() / 1000);
    datetime = "Boot+" + String(millis() / 1000) + "s";
  }
  
  // Add to detailed buffer (10-second intervals)
  detailedBuffer.push_back({now, t, h, datetime});
  
  // Keep only 10 minutes of detailed data
  while (detailedBuffer.size() > MAX_DETAILED_SAMPLES) {
    detailedBuffer.pop_front();
  }
  
  Serial.printf("✅ Reading [%s]: %.1f°C, %.0f%% RH (detailed: %d samples)\n", 
                datetime.c_str(), t, h, detailedBuffer.size());
  
  // Check for temperature alerts
  checkTemperatureAlert(t);
  
  // Aggregate old data every 5 minutes
  static uint32_t lastAggregation = 0;
  if ((now - lastAggregation) >= AGGREGATE_INTERVAL_SEC) {
    aggregateOldData();
    lastAggregation = now;
  }
}

void aggregateOldData() {
  // Only aggregate if we have enough detailed data
  if (detailedBuffer.empty()) return;
  
  // Get current time
  uint32_t now = getCurrentTimestamp();
  if (now == 0 || now < 1000000000) {
    now = millis() / 1000;
  }
  
  // Find data older than DETAILED_PERIOD_SEC (10 minutes)
  uint32_t cutoffTime = now - DETAILED_PERIOD_SEC;
  
  // Group old data into 5-minute buckets for aggregation
  std::map<uint32_t, std::vector<Reading>> buckets;
  
  auto it = detailedBuffer.begin();
  while (it != detailedBuffer.end() && it->ts < cutoffTime) {
    // Round timestamp to 5-minute boundary
    uint32_t bucketTime = (it->ts / AGGREGATE_INTERVAL_SEC) * AGGREGATE_INTERVAL_SEC;
    buckets[bucketTime].push_back(*it);
    ++it;
  }
  
  // Create aggregated readings from buckets
  for (const auto& bucket : buckets) {
    if (bucket.second.empty()) continue;
    
    float avgTemp = 0, avgHum = 0;
    for (const Reading& reading : bucket.second) {
      avgTemp += reading.t;
      avgHum += reading.h;
    }
    avgTemp /= bucket.second.size();
    avgHum /= bucket.second.size();
    
    // Create datetime string for aggregated data
    time_t bucketTimeT = bucket.first;
    struct tm timeinfo;
    localtime_r(&bucketTimeT, &timeinfo);
    char datetimeStr[64];
    strftime(datetimeStr, sizeof(datetimeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    // Add to aggregated buffer if not already present
    bool exists = false;
    for (const Reading& existing : aggregatedBuffer) {
      if (abs((int)existing.ts - (int)bucket.first) < 60) { // Within 1 minute tolerance
        exists = true;
        break;
      }
    }
    
    if (!exists) {
      aggregatedBuffer.push_back({bucket.first, avgTemp, avgHum, String(datetimeStr)});
      Serial.printf("Aggregated %d samples to 5-min avg: %.1f°C, %.0f%% RH [%s]\n", 
                   bucket.second.size(), avgTemp, avgHum, datetimeStr);
    }
  }
  
  // Keep only ~24 hours of aggregated data
  while (aggregatedBuffer.size() > MAX_AGGREGATE_SAMPLES) {
    aggregatedBuffer.erase(aggregatedBuffer.begin());
  }
  
  // Remove old detailed data that was aggregated
  it = detailedBuffer.begin();
  while (it != detailedBuffer.end() && it->ts < cutoffTime) {
    it = detailedBuffer.erase(it);
  }
  
  if (!buckets.empty()) {
    Serial.printf("Data aggregation complete: %d detailed + %d aggregated samples\n", 
                 detailedBuffer.size(), aggregatedBuffer.size());
  }
}

// Alert system functions
void checkTemperatureAlert(float temperature) {
  if (temperature > alertThreshold) {
    if (!alertActive) {
      // New alert triggered
      alertActive = true;
      alertAcknowledged = false;
      Serial.printf("TEMPERATURE ALERT! Current: %.1f°C, Threshold: %.1f°C\n", temperature, alertThreshold);
    }
  } else {
    // Temperature is below threshold
    if (alertActive) {
      alertActive = false;
      alertAcknowledged = true;
      Serial.println("Temperature alert cleared - back to normal");
    }
  }
}

void handleSetAlert(AsyncWebServerRequest *req) {
  if (req->hasParam("threshold")) {
    float newThreshold = req->getParam("threshold")->value().toFloat();
    if (newThreshold > 0 && newThreshold < 100) {
      alertThreshold = newThreshold;
      Serial.printf("Alert threshold set to: %.1f°C\n", alertThreshold);
      req->send(200, "application/json", "{\"status\":\"ok\",\"threshold\":" + String(alertThreshold) + "}");
    } else {
      req->send(400, "application/json", "{\"error\":\"Invalid threshold range (0-100°C)\"}");
    }
  } else {
    req->send(400, "application/json", "{\"error\":\"Missing threshold parameter\"}");
  }
}

void handleAckAlert(AsyncWebServerRequest *req) {
  if (alertActive) {
    alertAcknowledged = true;
    Serial.println("Temperature alert acknowledged by user");
    req->send(200, "application/json", "{\"status\":\"acknowledged\"}");
  } else {
    req->send(200, "application/json", "{\"status\":\"no_active_alert\"}");
  }
}

void handleGetAlert(AsyncWebServerRequest *req) {
  StaticJsonDocument<256> doc;
  doc["threshold"] = alertThreshold;
  doc["active"] = alertActive;
  doc["acknowledged"] = alertAcknowledged;
  doc["needs_attention"] = (alertActive && !alertAcknowledged);
  
  String output;
  serializeJson(doc, output);
  req->send(200, "application/json", output);
}

void handleCurrent(AsyncWebServerRequest *req) {
  if (detailedBuffer.empty()) {
    req->send(503, "application/json", "{\"error\":\"no data\"}");
    return;
  }
  
  StaticJsonDocument<256> doc;
  Reading last = detailedBuffer.back();
  doc["t"] = last.t;
  doc["h"] = last.h;
  doc["timestamp"] = last.ts;
  doc["datetime"] = last.datetime;
  doc["time_source"] = (last.ts > 1000000000) ? "NTP" : "boot_time";
  doc["sample_interval"] = SAMPLE_MS / 1000;
  doc["detailed_samples"] = detailedBuffer.size();
  doc["aggregated_samples"] = aggregatedBuffer.size();
  
  String output;
  serializeJson(doc, output);
  req->send(200, "application/json", output);
}

void handleHistory(AsyncWebServerRequest *req) {
  String range = "detailed";
  if (req->hasParam("range")) {
    range = req->getParam("range")->value();
  }
  
  DynamicJsonDocument doc(16384); // 16KB for JSON response
  JsonArray data = doc.createNestedArray("data");
  doc["sample_info"] = JsonObject();
  
  if (range == "detailed" || range == "10min") {
    // Return detailed 10-second data (last 10 minutes)
    doc["sample_info"]["type"] = "detailed";
    doc["sample_info"]["interval_seconds"] = SAMPLE_MS / 1000;
    doc["sample_info"]["max_age_minutes"] = DETAILED_PERIOD_SEC / 60;
    
    for (const auto& reading : detailedBuffer) {
      JsonObject obj = data.createNestedObject();
      obj["ts"] = reading.ts;
      obj["t"] = reading.t;
      obj["h"] = reading.h;
      obj["datetime"] = reading.datetime;
    }
  } else if (range == "aggregated" || range == "24h") {
    // Return aggregated 5-minute data
    doc["sample_info"]["type"] = "aggregated";
    doc["sample_info"]["interval_seconds"] = AGGREGATE_INTERVAL_SEC;
    doc["sample_info"]["max_age_hours"] = (MAX_AGGREGATE_SAMPLES * AGGREGATE_INTERVAL_SEC) / 3600;
    
    for (const auto& reading : aggregatedBuffer) {
      JsonObject obj = data.createNestedObject();
      obj["ts"] = reading.ts;
      obj["t"] = reading.t;
      obj["h"] = reading.h;
      obj["datetime"] = reading.datetime;
    }
  } else if (range == "all") {
    // Return combined data (aggregated + detailed)
    doc["sample_info"]["type"] = "combined";
    doc["sample_info"]["detailed_count"] = detailedBuffer.size();
    doc["sample_info"]["aggregated_count"] = aggregatedBuffer.size();
    
    // Add aggregated data first (older)
    for (const auto& reading : aggregatedBuffer) {
      JsonObject obj = data.createNestedObject();
      obj["ts"] = reading.ts;
      obj["t"] = reading.t;
      obj["h"] = reading.h;
      obj["datetime"] = reading.datetime;
      obj["type"] = "aggregated";
    }
    
    // Add detailed data last (newer)
    for (const auto& reading : detailedBuffer) {
      JsonObject obj = data.createNestedObject();
      obj["ts"] = reading.ts;
      obj["t"] = reading.t;
      obj["h"] = reading.h;
      obj["datetime"] = reading.datetime;
      obj["type"] = "detailed";
    }
  }
  
  String output;
  serializeJson(doc, output);
  req->send(200, "application/json", output);
}

void handleRoot(AsyncWebServerRequest *req) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>ESP32 Temperature & Humidity Monitor</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }
        .container { max-width: 1200px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }
        .current { background: #e8f4fd; padding: 15px; border-radius: 8px; margin-bottom: 20px; }
        .alert-section { background: #f8f9fa; padding: 15px; border-radius: 8px; margin-bottom: 20px; border: 2px solid #dee2e6; }
        .alert-active { background: #f8d7da !important; border-color: #dc3545 !important; }
        .alert-controls { display: flex; gap: 10px; align-items: center; margin-bottom: 10px; }
        .alert-status { font-weight: bold; }
        .alert-warning { background: #ffebee; color: #d32f2f; padding: 10px; border-radius: 5px; margin-top: 10px; display: none; }
        .alert-warning.show { display: block; animation: redAlert 1s infinite; }
        .charts { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; margin-top: 20px; }
        @keyframes redAlert { 
            0% { background: #ffebee; } 
            50% { background: #f44336; color: white; } 
            100% { background: #ffebee; } 
        }
        .chart-container { background: #fafafa; padding: 15px; border-radius: 8px; }
        select, input, button { padding: 8px; font-size: 14px; margin: 2px; }
        button { background: #007bff; color: white; border: none; border-radius: 4px; cursor: pointer; }
        button:hover { background: #0056b3; }
        button.danger { background: #dc3545; }
        button.danger:hover { background: #c82333; }
        canvas { max-height: 300px; }
        h1 { color: #333; text-align: center; }
        h2 { color: #666; margin: 0; }
        .loading { text-align: center; color: #666; }
        .blink { animation: blink 1s infinite; }
        @keyframes blink { 0% { opacity: 1; } 50% { opacity: 0.3; } 100% { opacity: 1; } }
    </style>
</head>
<body>
    <div class="container">
        <h1>Temperature & Humidity Monitor</h1>
        
        <div class="current">
            <h2 id="current-data" class="loading">Loading current data...</h2>
        </div>
        
        <div class="alert-section" id="alert-section">
            <h3>🌡️ Temperature Alert System</h3>
            <div class="alert-controls">
                <label for="alert-threshold">Alert Threshold:</label>
                <input type="number" id="alert-threshold" min="0" max="100" step="0.1" value="40.0" style="width: 80px;">
                <span>°C</span>
                <button onclick="setAlertThreshold()">Set Alert</button>
                <button onclick="testAlertSound()" style="background: #ff9800;">🔊 Test Sound</button>
                <button id="ack-button" onclick="acknowledgeAlert()" class="danger" style="display: none;">🔔 Acknowledge Alert</button>
            </div>
            <div class="alert-status" id="alert-status">Status: Loading...</div>
            <div class="alert-warning" id="alert-warning">
                <strong>🚨 RED ALERT! TEMPERATURE CRITICAL!</strong><br>
                Environmental systems report thermal threshold exceeded!<br>
                <em>Recommend immediate attention to environmental controls.</em>
            </div>
        </div>
        
        <div>
            <label for="range-select">Data View:</label>
            <select id="range-select">
                <option value="detailed">Detailed (30s intervals, last 30 min)</option>
                <option value="aggregated">Aggregated (5min intervals, ~24h)</option>
                <option value="all">Combined View (All Available Data)</option>
            </select>
            <span id="data-info" style="margin-left: 15px; color: #666; font-size: 12px;"></span>
        </div>
        
        <div class="charts">
            <div class="chart-container">
                <h3>Temperature (°C)</h3>
                <canvas id="temp-chart"></canvas>
            </div>
            <div class="chart-container">
                <h3>Humidity (%)</h3>
                <canvas id="humidity-chart"></canvas>
            </div>
        </div>
    </div>
    
    <!-- Multiple audio elements for maximum browser compatibility -->
    <audio id="alert-sound-1" preload="auto" volume="1.0">
        <!-- Simple beep sound that works everywhere -->
        <source src="data:audio/wav;base64,UklGRnoGAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQoGAACBhYqFbF1fdJivrJBhNjVgodDbq2EcBj+a2/LDciUFLIHO8tiJNwgZaLvt559NEAxQp+PwtmMcBjiR1/LMeSwFJHfH8N2QQAoUXrTp66hVFApGn+DyvmwhDy2JzfPYhT0F" type="audio/wav">
    </audio>
    <audio id="alert-sound-2" preload="auto" volume="1.0">
        <!-- Backup audio element -->
        <source src="data:audio/mpeg;base64,//uQRAAAAWMSLwUIYAAsYkXgoQwAEaYLWfkWgAI0wWs/ItAAAGDgYtAgAyN+QWaAAihwMWm4G8QQRDiMcCBcH3Cc+CDv/7xA4Tvh9Rz/y8QADBwMWgQAZG/ILNAARQ4GLTcDeIIIhxGOBAuD7hOfBB3/94gcJ3w+o5/5eIAIAAAVwWgQAVQ2ORaIQwEMAJiDg95G4nQL7mQVWI6GwRcfsZAcsKkJvxgxEjzFUgfHoSQ9Qq7KNwqHwuB13MA4a1q/DmBrHgPcmjiGoh//EwC5nGPEmS4RcfkVKOhJf+WOgoxJclFz3kgn//dBA+ya1GhurNn8zb//9NNutNuhz31f////9vt///z+IdAEAAAK4LQIAKobHItEIYCGAExBwe8jcToF9zIKrEdDYIuP2MgOWFSE34wYiR5iqQPj0JIeoVdlG4VD4XA67mAcNa1fhzA1jwHuTRxDUQ//iYBczjHiTJcIuPyKlHQkv/LHQUYkuSi57yQT//uggfZNajQ3Vmz+Zt//+mm3Wm3Q576v////+32///5/EOgAAADVghQAAAAA//uQZAUAB1WI0PZugAAAAAoQwAAAEk3nRd2qAAAAACiDgAAAAAAABCqEEQRLCgwpBGMlJkIz8jKhGvj4k6jzRnqasNKIeRxN6kFZdRdaQZKB49kPRV4DzP8k08FYgC12n3hL2S5Qy9fFZcnKzq8zT3vQVRRi1RCsG8MIQLwRX8i5rKvZxBQTNlGOjUmq4UIKWgL8xt7qVnGqUDtEm7NheBz6VCTB1CDbNVfHQ9FQE0QvlMdVcInU6ek5DjKNaT3WdHdH/3XLgPZFOX3PjZtH2PVVK9VXyJ1iy9qhDXQOeDqF8EWH8QE8TRo08qZN3M4/nqGCGzOYm5yKPbLBGu4lllYQgb8z8Kt8fNSRFKtY6SyYM4Jz+o8zzxAoZuU0J9JZEEiPJ/cLPDxR/B5sEd+Gvj9RLPx+jMwWfPkIBOPwbHHWpAAU8nZWGo9dCQMGKA8v/LOQOzQiE4U3gAABBweUPZSxKC8lKSjJZi0HbebJPIIyZNJVxMrLFElKEDw1vXVTUwNmFmfCwlCFKEHOPBmFJWQQSGC2EXMApCBoQYM3l6GS8FGhRyGQJqGZE8EMXcPcGQAF0QgogfLXL5nTjOLwIDQMPQyNbGQQkm6pOOgOqYeAqCCjl2AAE4OPV1PBVNAXFRwChJAKhI1TuVGQAABG2wBQoP8IKhIohQjvPKg3r3i4pL0YBTWKrBDO+c3BLShCDjVKQPvYXkhgwOLEg3kNy1D2qBMgQTqABagAz3YBa7Tdi8wOIYNpCmyYlJCfClHWhyJXJM/TwLUC4LArCwCAaIEUCJKNl5uQkNJOJ+xFYlZVpNb1lxJmGkD7+D6lKuPfJvJ4MfJPnGnY0i44vAAAYfhTYBcKYb2+Kx2qP74OAACAD8xAIBxwSjLoRVLQGUUYLAUOAKzUC+4A9oAJRjEGdQEGQQKiLJMHUUkMGFBSTJJAI1MIWGUZWHQsIhFD8BKSJZDZFMGFoOyxMkKOKqJlAJrYxAjYSyPYUmfAD8CRUdaQQmqJXIDzf+4CjMPBwkKxEFVlpBBfAxcZKQ//uQZB0AB9mXUdLQ2gAAAAAAAAAAAEKkQRBEsKDCkEYyUmQjPyMqEa+PiTqPNGeprI2Rxa9iCqJKZBgNH3KQ/5pS/1p8kQgSGUIhAk7qI3yOEMKSkJJaP5N7N3wKWFDWKJKVFJKRkrWTQS7mPJvMjNr6UVWAqCZokLY1GWWq7RimIFrHU+d6ZmHdCppf1qzJJzGozQhUCMNJuYSEQG8QVggAhJgHmqBJb85SvgKKzCHkfZOMzKyTlW3cLVUl4Epo7CyYOFqKjTJIUrqwlxbLGY1Q0Cl1DFCC9oUYdKkYTFOKmZOwE6A9DzVJHuS5JBQyFKQ5KZoVWqJ4YXgAy7OC0dajMKI+yl46a+KX8bsGGRNO/LE+VjBBRkKF1MIQ1Bj2zRnPLwF//2Q5BIABlmXQdLM1gAAAAAAAAAAAJEKYQRFEsKDCkEYyUmQjPyMqEa+PiTqPNGeprqVPNz3FrJNbdL5PY5N7WyfOgUrVr5bQ47G3dxOvQiSkJNAqw5d3QWwb5xKP24/3GIkDV3fJNAGU3qbfxz+0Ybwoz+hqO8SPMGQgA6G8nRaHE0BbwL3BqTJo1KBpaNRFJQKOKN1NKZAR6JQGJG9CIKSqQMcCRArPP71DFY8Tg5JZsYo9C0nwGaFMo2qJIoHKPNgU6KYXYgJqvZKp8EQM6hqPJqnYe8pnLNygJ1BBaBdU+v/uQZAgABdZ0zQCMYgAAAAAAAAAAQ45BCEjEYTHg5c8JGWOgBGYwVs0KqVIsJ2n8/VN8KJppZH/H5f/wTjV6h6L+lKOg6LHYPkWAzJCkKZc9dFMo0nPwYQNaVJy1lT/qR+c8h5VDrEgPCHd0I6pjH/KQNXyQDQJ4ePrfFP1I5XTcvqq8R8VQQSGArCjZIqCo0NW7fPRgKcUJjYjORpV9kz0KzOm9rVgqx7TGcFAXDZkSl2AxJdpUbD/XGtGnA4+GVOQGQCKjqvKfBlHfzVlHzLZIE/MQVEgDEOYtJJZmWsrK8Y3UU0vgOKQP/2Q5AcABc5WzUAYsiAAAAAAAAAAAAEJEQRFEsKDCkEYyUmQjPyMqEa+PiTqPNGepqqO5sqRCGS7ZU7JCJWKdTJWtklIR4F7NB1TJKYnMgmLrJDDNsUesTJZwQiD0YYjjwQlqPSPEJdCCILOj0o5TKBHJyNRIzgKVHaNxJQDJ4z9Dyl3BwZZuJUATAzlF+y+Y3RmOZ2oVKhJGQu4OIohQAJXJNVkKBjMkQJJI4pG+LnlCH3K0UXw1H3UpJXHCELVNqUohxVdNDKdRIyDFcATOJ1Xog1QYqJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJRBEsKDCkEYyUmQjPyMqEa+PiTqPNGeprFJRbVhKqoaZ1uGmUg3WU5mKdl6RL3JWKoaF9GtdWYLcEGwJL6q65h8NlRnlYgRFQ2O5+wBmQSHuO8zGJDADEdvYGfJLYZCPMzBxP//uQZAcAD4FPMKMbVgAAAAAAAAAAAEKckRBFsXgEIRYQW4yyAYsKJDz8lNnQmlKTlqp1EJmVWl6yKJvEtVJJJJJJJJJJJJJVnFxCF0AkhHEhLzKJJwNqp3dKJ1tZY5pWkJlZhFQhJp1kVkqkVNKqjzKz9kYP1Y8UkZQSXnSzg6T3bqazT14mISNFHnQfUvZD4t9nD84jRU0qQ3gJOB/8ZqVWdnT8Qpb7xdvVzS2Ks5h7YV0nYf8CgkZ4O31I87vOT8H/K5JOzh7vWr9VL7BkdZW+JqJ5j9zKz//uQZAYAD4FHMD3M1gAAAAAAAAAAAEKMQRBFsEBCk8THDUI5sHGmJWNKKZolWlBjIgz5YqJFcKJDWVNRQjEhE+KqxUhBzjcXgMXTqJFfcpGQ45MXj5gV5s3YSVP8z0gApz/gzUwpJFtNJHKvqvJ7zQNnKUlbw01kAy2k0LoNHnZgQKBJIEVLyUP8QJFlMFwQKUQSo0wQlq6K0NGZ3KYKAQwMkQQGlKOTGJGK8+tl8iJFT7gEbpSjJFqShU0I8aPCDFR8XYk//2Q9AwAH8AcgHLhyAAAAAAAAAAAAEaggWqP8xm+T45Y5CuLz86z8t8opU8zGLqwJOg7e6I/aZMOoJbS" type="audio/mpeg">
    </audio>
    
    <!-- Simple audio generation script -->
    <script>
        let audioContext = null;
        let alertInterval = null;
        
        function initSimpleAudio() {
            try {
                audioContext = new (window.AudioContext || window.webkitAudioContext)();
                return true;
            } catch (e) {
                console.log('Web Audio API not supported');
                return false;
            }
        }
        
        function playSimpleBeep(frequency = 800, duration = 500, volume = 0.8) {
            if (!audioContext) {
                if (!initSimpleAudio()) return false;
            }
            
            // Resume audio context if suspended
            if (audioContext.state === 'suspended') {
                audioContext.resume();
            }
            
            const oscillator = audioContext.createOscillator();
            const gainNode = audioContext.createGain();
            
            oscillator.connect(gainNode);
            gainNode.connect(audioContext.destination);
            
            oscillator.frequency.value = frequency;
            oscillator.type = 'square'; // Square wave for loud, clear sound
            
            gainNode.gain.setValueAtTime(0, audioContext.currentTime);
            gainNode.gain.linearRampToValueAtTime(volume, audioContext.currentTime + 0.01);
            gainNode.gain.exponentialRampToValueAtTime(0.01, audioContext.currentTime + duration / 1000);
            
            oscillator.start(audioContext.currentTime);
            oscillator.stop(audioContext.currentTime + duration / 1000);
            
            return true;
        }
        
        function playAlertPattern() {
            // Play RED ALERT pattern: High-Low-High-Low
            playSimpleBeep(1000, 300, 0.9); // High frequency, loud
            setTimeout(() => playSimpleBeep(600, 300, 0.9), 350);  // Low frequency
            setTimeout(() => playSimpleBeep(1000, 300, 0.9), 700); // High again
            setTimeout(() => playSimpleBeep(600, 300, 0.9), 1050); // Low again
        }
        
        function playHTMLAudio() {
            // Try HTML5 audio as fallback
            const audio1 = document.getElementById('alert-sound-1');
            const audio2 = document.getElementById('alert-sound-2');
            
            // Set volume to maximum
            if (audio1) {
                audio1.volume = 1.0;
                audio1.play().catch(() => {
                    if (audio2) {
                        audio2.volume = 1.0;
                        audio2.play().catch(() => console.log('All audio fallbacks failed'));
                    }
                });
            }
        }
    </script>

    <script>
        let tempChart, humidityChart;
        let isAlertSounding = false;
        
        async function fetchJson(url) {
            try {
                const response = await fetch(url);
                return await response.json();
            } catch (error) {
                console.error('Fetch error:', error);
                return null;
            }
        }
        
        async function postData(url, data) {
            try {
                const response = await fetch(url, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: new URLSearchParams(data)
                });
                return await response.json();
            } catch (error) {
                console.error('Post error:', error);
                return null;
            }
        }
        
        // Alert system functions
        async function updateAlertStatus() {
            const alertData = await fetchJson('/api/alert/get');
            if (alertData) {
                document.getElementById('alert-threshold').value = alertData.threshold.toFixed(1);
                
                const statusEl = document.getElementById('alert-status');
                const sectionEl = document.getElementById('alert-section');
                const warningEl = document.getElementById('alert-warning');
                const ackButton = document.getElementById('ack-button');
                
                if (alertData.needs_attention) {
                    // Alert is active and not acknowledged
                    statusEl.innerHTML = '🚨 <span class="blink">ALERT ACTIVE</span> - Temperature exceeds threshold!';
                    sectionEl.classList.add('alert-active');
                    warningEl.classList.add('show');
                    ackButton.style.display = 'inline-block';
                    
                    // Play alert sound if not already playing
                    if (!isAlertSounding) {
                        playAlertSound();
                    }
                } else if (alertData.active && alertData.acknowledged) {
                    statusEl.innerHTML = '⚠️ Alert acknowledged - Temperature still above threshold';
                    sectionEl.classList.add('alert-active');
                    warningEl.classList.remove('show');
                    ackButton.style.display = 'none';
                    stopAlertSound();
                } else {
                    statusEl.innerHTML = '✅ Normal - Temperature within limits';
                    sectionEl.classList.remove('alert-active');
                    warningEl.classList.remove('show');
                    ackButton.style.display = 'none';
                    stopAlertSound();
                }
            }
        }
        
        function playAlertSound() {
            if (!isAlertSounding) {
                isAlertSounding = true;
                console.log('🚨 RED ALERT! Temperature threshold exceeded!');
                
                // Try Web Audio API first (loudest option)
                const webAudioSuccess = playSimpleBeep(1000, 500, 1.0);
                if (!webAudioSuccess) {
                    // Fallback to HTML5 audio
                    playHTMLAudio();
                }
                
                // Start repeating alert pattern
                alertInterval = setInterval(() => {
                    if (isAlertSounding) {
                        // Try Web Audio API pattern
                        if (!playAlertPattern()) {
                            // Fallback to HTML5 audio
                            playHTMLAudio();
                        }
                    }
                }, 2000); // Repeat every 2 seconds
            }
        }
        
        function stopAlertSound() {
            if (isAlertSounding) {
                isAlertSounding = false;
                
                // Stop the repeating alert
                if (alertInterval) {
                    clearInterval(alertInterval);
                    alertInterval = null;
                }
                
                // Stop any HTML5 audio
                const audio1 = document.getElementById('alert-sound-1');
                const audio2 = document.getElementById('alert-sound-2');
                if (audio1) audio1.pause();
                if (audio2) audio2.pause();
                
                console.log('✅ Alert acknowledged - All clear');
            }
        }
        
        async function setAlertThreshold() {
            const threshold = document.getElementById('alert-threshold').value;
            const result = await postData('/api/alert/set', { threshold: threshold });
            if (result && result.status === 'ok') {
                console.log('Alert threshold set to:', threshold);
                updateAlertStatus();
            } else {
                alert('Failed to set alert threshold');
            }
        }
        
        async function acknowledgeAlert() {
            const result = await postData('/api/alert/acknowledge', {});
            if (result) {
                console.log('Alert acknowledged');
                stopAlertSound();
                updateAlertStatus();
            }
        }
        
        function testAlertSound() {
            console.log('🔊 Testing alert sound...');
            
            // Stop any current alert
            stopAlertSound();
            
            // Play test sound
            isAlertSounding = true;
            
            // Try Web Audio API first
            const webAudioSuccess = playSimpleBeep(1000, 800, 1.0);
            if (!webAudioSuccess) {
                console.log('Web Audio failed, trying HTML5 audio...');
                playHTMLAudio();
            }
            
            // Play full alert pattern once
            setTimeout(() => {
                if (!playAlertPattern()) {
                    playHTMLAudio();
                }
            }, 1000);
            
            // Stop test after 5 seconds
            setTimeout(() => {
                isAlertSounding = false;
                stopAlertSound();
                console.log('🔇 Sound test completed');
            }, 5000);
        }
        
        async function updateCurrent() {
            const current = await fetchJson('/api/current');
            if (current && !current.error) {
                const timeInfo = current.time_source === 'NTP' ? current.datetime : 
                                current.time_source === 'boot_time' ? 'Time since boot: ' + current.datetime : 
                                'No time sync';
                
                const sampleInfo = `${current.sample_interval}s intervals | ${current.detailed_samples} detailed + ${current.aggregated_samples} aggregated samples`;
                
                document.getElementById('current-data').innerHTML = 
                    `Current: <strong>${current.t.toFixed(1)}°C</strong> | <strong>${current.h.toFixed(0)}%</strong> RH<br>
                     <small>📅 ${timeInfo}</small><br>
                     <small>📊 ${sampleInfo}</small>`;
            }
            
            // Update alert status
            updateAlertStatus();
        }
        
        async function updateCharts() {
            const range = document.getElementById('range-select').value;
            const history = await fetchJson('/api/history?range=' + range);
            
            if (!history || !history.data) return;
            
            // Update data info display
            const dataInfoEl = document.getElementById('data-info');
            if (history.sample_info) {
                if (history.sample_info.type === 'detailed') {
                    dataInfoEl.textContent = `${history.data.length} samples, ${history.sample_info.interval_seconds}s intervals`;
                } else if (history.sample_info.type === 'aggregated') {
                    dataInfoEl.textContent = `${history.data.length} samples, ${history.sample_info.interval_seconds}s intervals`;
                } else if (history.sample_info.type === 'combined') {
                    dataInfoEl.textContent = `${history.sample_info.detailed_count} detailed + ${history.sample_info.aggregated_count} aggregated`;
                }
            }
            
            // Format timestamps based on data source
            const labels = history.data.map(item => {
                if (item.ts > 1000000000) {
                    // Real timestamp - show time appropriately
                    const date = new Date(item.ts * 1000);
                    if (range === 'detailed') {
                        return date.toLocaleTimeString(); // Show time only for detailed view
                    } else {
                        return date.toLocaleString(); // Show date and time for aggregated
                    }
                } else {
                    // Boot time - show relative time
                    return `+${item.ts}s`;
                }
            });
            
            const temps = history.data.map(item => item.t);
            const humidity = history.data.map(item => item.h);
            
            // Destroy existing charts
            if (tempChart) tempChart.destroy();
            if (humidityChart) humidityChart.destroy();
            
            // Calculate dynamic temperature range
            const minTemp = Math.min(...temps);
            const maxTemp = Math.max(...temps);
            const tempRange = maxTemp - minTemp;
            const tempPadding = Math.max(1, tempRange * 0.1); // 10% padding, minimum 1°C
            
            // Temperature chart
            tempChart = new Chart(document.getElementById('temp-chart'), {
                type: 'line',
                data: {
                    labels: labels,
                    datasets: [{
                        label: 'Temperature (°C)',
                        data: temps,
                        borderColor: 'rgb(255, 99, 132)',
                        backgroundColor: 'rgba(255, 99, 132, 0.1)',
                        tension: 0.1
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: true,
                    scales: {
                        y: {
                            min: minTemp - tempPadding,
                            max: maxTemp + tempPadding,
                            ticks: {
                                callback: function(value) {
                                    return value.toFixed(1) + '°C';
                                }
                            }
                        }
                    }
                }
            });
            
            // Calculate dynamic humidity range
            const minHum = Math.min(...humidity);
            const maxHum = Math.max(...humidity);
            const humRange = maxHum - minHum;
            const humPadding = Math.max(2, humRange * 0.1); // 10% padding, minimum 2%
            
            // Keep humidity within reasonable bounds (0-100%)
            const humMin = Math.max(0, minHum - humPadding);
            const humMax = Math.min(100, maxHum + humPadding);
            
            // Humidity chart
            humidityChart = new Chart(document.getElementById('humidity-chart'), {
                type: 'line',
                data: {
                    labels: labels,
                    datasets: [{
                        label: 'Humidity (%)',
                        data: humidity,
                        borderColor: 'rgb(54, 162, 235)',
                        backgroundColor: 'rgba(54, 162, 235, 0.1)',
                        tension: 0.1
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: true,
                    scales: {
                        y: {
                            min: humMin,
                            max: humMax,
                            ticks: {
                                callback: function(value) {
                                    return value.toFixed(0) + '%';
                                }
                            }
                        }
                    }
                }
            });
        }
        
        // Event listeners
        document.getElementById('range-select').addEventListener('change', updateCharts);
        
        // Initial load
        updateCurrent();
        updateCharts();
        updateAlertStatus();
        
        // Update current data and alerts every 30 seconds (matches sensor sampling)
        setInterval(updateCurrent, 30000);
        
        // Update charts every 30 seconds for detailed view, less frequently for others
        setInterval(() => {
            const range = document.getElementById('range-select').value;
            if (range === 'detailed') {
                updateCharts(); // Update frequently for detailed view
            }
        }, 30000);
        
        // Update charts for aggregated views every 5 minutes
        setInterval(() => {
            const range = document.getElementById('range-select').value;
            if (range !== 'detailed') {
                updateCharts();
            }
        }, 300000);
    </script>
</body>
</html>
)rawliteral";
  
  req->send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("ESP32 Temperature/Humidity Logger Starting...");
  
  // Initialize DHT sensor
  dht.begin();
  Serial.println("DHT11 sensor initialized on GPIO4");
  
  // Initialize SPIFFS (not used in this simple version)
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed");
  }
  
  // Network initialization with discovery features
  Serial.println("Connecting to network...");
  blinkStatusLED(2, 500); // 2 blinks = trying to connect
  
  if (USE_ETH) {
    Serial.println("Initializing Ethernet...");
    ETH.begin();
    
    // Wait for Ethernet connection
    int eth_timeout = 0;
    while (ETH.linkUp() == false && eth_timeout < 20) {
      delay(1000);
      eth_timeout++;
      Serial.print(".");
    }
    
    if (ETH.linkUp()) {
      Serial.println("\nEthernet connected!");
      isConnected = true;
      setupNetworkDiscovery();
      printNetworkInfo();
    } else {
      Serial.println("\nEthernet failed, falling back to WiFi");
      WiFi.setHostname(HOSTNAME);
      WiFi.begin(SSID, PASS);
      while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.print(".");
      }
      Serial.println("\nWiFi connected!");
      isConnected = true;
      setupNetworkDiscovery();
      printNetworkInfo();
    }
  } else {
    Serial.println("Using WiFi only...");
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(SSID, PASS);
    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
      Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
    isConnected = true;
    setupNetworkDiscovery();
    printNetworkInfo();
  }
  
  // Setup NTP time synchronization
  setupNTP();
  
  // Setup web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/current", HTTP_GET, handleCurrent);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/alert/get", HTTP_GET, handleGetAlert);
  server.on("/api/alert/set", HTTP_POST, handleSetAlert);
  server.on("/api/alert/acknowledge", HTTP_POST, handleAckAlert);
  
  // Start server
  server.begin();
  Serial.println("Web server started");
  
  // Initial sensor reading
  delay(2000);  // DHT needs time to stabilize
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  addReading(t, h);
  
  Serial.println("Setup complete!");
}

void loop() {
  // Check network status periodically
  if (millis() - lastNetworkCheck >= NETWORK_CHECK_MS) {
    lastNetworkCheck = millis();
    checkNetworkStatus();
  }
  
  // Take sensor readings
  if (millis() - lastSample >= SAMPLE_MS) {
    lastSample = millis();
    
    Serial.println("🌡️ Reading DHT sensor...");
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    
    Serial.printf("🔍 Raw DHT values: T=%.2f°C, H=%.2f%%\n", temperature, humidity);
    addReading(temperature, humidity);
    
    // Blink LED to show activity
    if (isConnected) {
      blinkStatusLED(1, 50); // Quick blink on successful reading
    }
  }
  
  delay(100);  // Small delay to prevent watchdog issues
} 