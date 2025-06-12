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

// Memory management and persistence configuration
constexpr uint32_t EMERGENCY_AGGREGATION_THRESHOLD = 80; // Start emergency aggregation at 80% RAM usage
constexpr uint32_t CRITICAL_MEMORY_THRESHOLD = 90;       // Critical memory usage (force cleanup)
constexpr uint32_t SPIFFS_SAVE_INTERVAL_SEC = 3600;      // Save to flash every hour
constexpr uint32_t MAX_SPIFFS_RECORDS = 2016;            // 7 days * 24h * 12 (5-min intervals)
const char* SPIFFS_DATA_FILE = "/sensor_data.json";
const char* SPIFFS_CONFIG_FILE = "/config.json";

// Memory usage tracking
uint32_t lastMemoryCheck = 0;
uint32_t lastSPIFFSSave = 0;
bool emergencyMode = false;

struct Reading { 
  uint32_t ts;          // Unix timestamp (seconds since 1970)
  float t;              // Temperature in Celsius
  float h;              // Humidity in %
  String datetime;      // Human-readable date/time string
};

// Alert system variables
float alertThreshold = 40.0;     // Default alert temperature in Celsius
float humidityAlertThreshold = 90.0;  // Default alert humidity in %
bool alertActive = false;        // Is an alert currently active?
bool alertAcknowledged = true;   // Has the current alert been acknowledged?
bool humidityAlertActive = false;        // Is a humidity alert currently active?
bool humidityAlertAcknowledged = true;   // Has the current humidity alert been acknowledged?
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
void emergencyDataCompression();
void saveToPersistentStorage();
void loadFromPersistentStorage();
void saveConfigToPersistentStorage();
void loadConfigFromPersistentStorage();
uint32_t getMemoryUsagePercent();
void checkMemoryUsage();
void setupNTP();
String getCurrentDateTime();
uint32_t getCurrentTimestamp();
void checkTemperatureAlert(float temperature);
void checkHumidityAlert(float humidity);
void handleSetAlert(AsyncWebServerRequest *req);
void handleSetHumidityAlert(AsyncWebServerRequest *req);
void handleAckAlert(AsyncWebServerRequest *req);
void handleAckHumidityAlert(AsyncWebServerRequest *req);
void handleGetAlert(AsyncWebServerRequest *req);
void handleGetHumidityAlert(AsyncWebServerRequest *req);
void handleCurrent(AsyncWebServerRequest *req);
void handleHistory(AsyncWebServerRequest *req);
void handleRoot(AsyncWebServerRequest *req);

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
  
  // Check memory usage every reading
  checkMemoryUsage();
  
  // Aggregate old data every 5 minutes (or sooner if in emergency mode)
  static uint32_t lastAggregation = 0;
  uint32_t aggregationInterval = emergencyMode ? (AGGREGATE_INTERVAL_SEC / 2) : AGGREGATE_INTERVAL_SEC;
  if ((now - lastAggregation) >= aggregationInterval) {
    aggregateOldData();
    lastAggregation = now;
  }
  
  // Save to persistent storage periodically
  if ((now - lastSPIFFSSave) >= SPIFFS_SAVE_INTERVAL_SEC) {
    saveToPersistentStorage();
    lastSPIFFSSave = now;
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

// Memory management and persistent storage functions
uint32_t getMemoryUsagePercent() {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = ESP.getHeapSize();
  uint32_t usedHeap = totalHeap - freeHeap;
  return (usedHeap * 100) / totalHeap;
}

void checkMemoryUsage() {
  uint32_t memUsage = getMemoryUsagePercent();
  
  if (memUsage >= CRITICAL_MEMORY_THRESHOLD) {
    Serial.printf("🚨 CRITICAL MEMORY: %d%% used - Emergency cleanup!\n", memUsage);
    emergencyDataCompression();
    emergencyMode = true;
  } else if (memUsage >= EMERGENCY_AGGREGATION_THRESHOLD) {
    if (!emergencyMode) {
      Serial.printf("⚠️ HIGH MEMORY: %d%% used - Starting emergency aggregation\n", memUsage);
      emergencyDataCompression();
      emergencyMode = true;
    }
  } else {
    if (emergencyMode) {
      Serial.printf("✅ Memory normal: %d%% used - Exiting emergency mode\n", memUsage);
      emergencyMode = false;
    }
  }
}

void emergencyDataCompression() {
  Serial.println("🔄 Emergency data compression starting...");
  
  // Aggressively reduce detailed buffer
  while (detailedBuffer.size() > (MAX_DETAILED_SAMPLES / 2)) {
    detailedBuffer.pop_front();
  }
  
  // Reduce aggregated buffer if still critical
  while (aggregatedBuffer.size() > (MAX_AGGREGATE_SAMPLES / 2) && getMemoryUsagePercent() > CRITICAL_MEMORY_THRESHOLD) {
    aggregatedBuffer.erase(aggregatedBuffer.begin());
  }
  
  // Force garbage collection
  heap_caps_check_integrity_all(true);
  
  Serial.printf("✅ Emergency compression complete: %d detailed + %d aggregated samples remain\n", 
                detailedBuffer.size(), aggregatedBuffer.size());
}

void saveToPersistentStorage() {
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS mount failed - data not saved");
    return;
  }
  
  Serial.println("💾 Saving data to persistent storage...");
  
  // Create JSON document for aggregated data only (detailed data is temporary)
  DynamicJsonDocument doc(32768); // 32KB for JSON
  JsonArray dataArray = doc.createNestedArray("aggregated_data");
  
  // Save last MAX_SPIFFS_RECORDS of aggregated data
  size_t startIdx = 0;
  if (aggregatedBuffer.size() > MAX_SPIFFS_RECORDS) {
    startIdx = aggregatedBuffer.size() - MAX_SPIFFS_RECORDS;
  }
  
  for (size_t i = startIdx; i < aggregatedBuffer.size(); i++) {
    JsonObject reading = dataArray.createNestedObject();
    reading["ts"] = aggregatedBuffer[i].ts;
    reading["t"] = aggregatedBuffer[i].t;
    reading["h"] = aggregatedBuffer[i].h;
    reading["dt"] = aggregatedBuffer[i].datetime;
  }
  
  // Add metadata
  doc["last_save"] = getCurrentTimestamp();
  doc["version"] = "1.0";
  doc["total_records"] = dataArray.size();
  
  // Write to file
  File file = SPIFFS.open(SPIFFS_DATA_FILE, "w");
  if (!file) {
    Serial.println("❌ Failed to open data file for writing");
    return;
  }
  
  size_t written = serializeJson(doc, file);
  file.close();
  
  Serial.printf("✅ Saved %d aggregated records (%d bytes) to persistent storage\n", 
                dataArray.size(), written);
  
  // Save configuration too
  saveConfigToPersistentStorage();
}

void loadFromPersistentStorage() {
  if (!SPIFFS.begin(true)) {
    Serial.println("⚠️ SPIFFS mount failed - no persistent data loaded");
    return;
  }
  
  // Load historical data
  File file = SPIFFS.open(SPIFFS_DATA_FILE, "r");
  if (!file) {
    Serial.println("ℹ️ No previous data file found - starting fresh");
    return;
  }
  
  Serial.println("📂 Loading data from persistent storage...");
  
  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.printf("❌ Failed to parse data file: %s\n", error.c_str());
    return;
  }
  
  // Load aggregated data
  JsonArray dataArray = doc["aggregated_data"];
  int loadedCount = 0;
  
  for (JsonObject reading : dataArray) {
    uint32_t ts = reading["ts"];
    float t = reading["t"];
    float h = reading["h"];
    String dt = reading["dt"].as<String>();
    
    // Only load data that's not too old (within 7 days)
    uint32_t now = getCurrentTimestamp();
    if (now == 0) now = millis() / 1000; // Fallback to boot time
    
    if ((now - ts) <= (7 * 24 * 3600)) { // Within 7 days
      aggregatedBuffer.push_back({ts, t, h, dt});
      loadedCount++;
    }
  }
  
  Serial.printf("✅ Loaded %d historical records from persistent storage\n", loadedCount);
  
  // Load configuration
  loadConfigFromPersistentStorage();
}

void saveConfigToPersistentStorage() {
  if (!SPIFFS.begin(true)) return;
  
  DynamicJsonDocument doc(512);
  doc["alert_threshold"] = alertThreshold;
  doc["last_save"] = getCurrentTimestamp();
  doc["version"] = "1.0";
  
  File file = SPIFFS.open(SPIFFS_CONFIG_FILE, "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.println("💾 Configuration saved to persistent storage");
  }
}

void loadConfigFromPersistentStorage() {
  if (!SPIFFS.begin(true)) return;
  
  File file = SPIFFS.open(SPIFFS_CONFIG_FILE, "r");
  if (!file) return;
  
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (!error && doc.containsKey("alert_threshold")) {
    alertThreshold = doc["alert_threshold"];
    Serial.printf("📂 Loaded alert threshold: %.1f°C from persistent storage\n", alertThreshold);
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
      saveConfigToPersistentStorage(); // Save config immediately
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

void handleSaveData(AsyncWebServerRequest *req) {
  saveToPersistentStorage();
  StaticJsonDocument<256> doc;
  doc["status"] = "success";
  doc["message"] = "Data saved to persistent storage";
  doc["records_saved"] = aggregatedBuffer.size();
  doc["memory_usage"] = getMemoryUsagePercent();
  
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
  doc["memory_usage_percent"] = getMemoryUsagePercent();
  doc["free_heap_kb"] = ESP.getFreeHeap() / 1024;
  doc["emergency_mode"] = emergencyMode;
  doc["persistent_storage"] = SPIFFS.begin(false);
  
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
            <div id="system-status" style="margin-top: 10px; font-size: 12px; color: #666;">
                <span id="memory-status">Memory: --</span> | 
                <span id="storage-status">Storage: --</span> | 
                <span id="emergency-status">Mode: --</span>
            </div>
        </div>
        
        <div class="alert-section" id="alert-section">
            <h3>🌡️ Temperature Alert System</h3>
            <div class="alert-controls">
                <label for="alert-threshold">Alert Threshold:</label>
                <input type="number" id="alert-threshold" min="0" max="100" step="0.1" value="40.0" style="width: 80px;">
                <span>°C</span>
                <button onclick="setAlertThreshold()">Set Alert</button>
                <button id="ack-button" onclick="acknowledgeAlert()" class="danger" style="display: none;">🔔 Acknowledge Alert</button>
            </div>
            <div id="audio-test-section" style="margin-top: 10px;">
                <button onclick="testAlertSound()" style="background: #ff9800; font-size: 16px; padding: 10px 15px; font-weight: bold;">🔊 TEST SOUND (Click First!)</button>
                <button onclick="testVoiceOnly()" style="background: #2196f3; margin-left: 10px;">🗣️ Test Voice</button>
                <button onclick="showAvailableVoices()" style="background: #4caf50; margin-left: 10px;">📋 Show Voices</button>
                <br><small style="color: #666;">⚠️ Must test sound first to enable audio alerts</small>
            </div>
            <div id="audio-ready-section" style="margin-top: 10px; display: none; background: #d4edda; border: 2px solid #28a745; padding: 10px; border-radius: 5px;">
                <span style="color: #155724; font-weight: bold;">✅ Audio System Ready</span>
                <button onclick="testAlertSound()" style="background: #28a745; margin-left: 10px;">🔊 Test Again</button>
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
    
    <!-- Ultra simple audio that works everywhere -->
    <script>
        let audioContext = null;
        let isAudioEnabled = false;
        
                 // Initialize audio on first user click (required by browsers)
         function enableAudio() {
             if (isAudioEnabled) {
                 // If already enabled, show green status
                 document.getElementById('audio-test-section').style.display = 'none';
                 document.getElementById('audio-ready-section').style.display = 'block';
                 return true;
             }
             
             try {
                 audioContext = new (window.AudioContext || window.webkitAudioContext)();
                 
                 // Play a silent sound to unlock audio
                 const oscillator = audioContext.createOscillator();
                 const gainNode = audioContext.createGain();
                 oscillator.connect(gainNode);
                 gainNode.connect(audioContext.destination);
                 gainNode.gain.value = 0.001; // Almost silent
                 oscillator.frequency.value = 440;
                 oscillator.start();
                 oscillator.stop(audioContext.currentTime + 0.01);
                 
                 isAudioEnabled = true;
                 console.log('✅ Audio enabled!');
                 return true;
             } catch (e) {
                 console.log('❌ Audio failed to initialize:', e);
                 return false;
             }
         }
        
        // Super simple, loud beep
        function playLoudBeep(frequency = 1000, duration = 500) {
            if (!isAudioEnabled) {
                console.log('Audio not enabled - click Test Sound first');
                return false;
            }
            
            if (!audioContext || audioContext.state === 'closed') {
                console.log('Audio context not available');
                return false;
            }
            
            try {
                // Resume if suspended
                if (audioContext.state === 'suspended') {
                    audioContext.resume();
                }
                
                const oscillator = audioContext.createOscillator();
                const gainNode = audioContext.createGain();
                
                oscillator.connect(gainNode);
                gainNode.connect(audioContext.destination);
                
                oscillator.type = 'square';
                oscillator.frequency.value = frequency;
                
                // LOUD volume
                gainNode.gain.setValueAtTime(0, audioContext.currentTime);
                gainNode.gain.linearRampToValueAtTime(0.5, audioContext.currentTime + 0.01); // 50% volume = very loud
                gainNode.gain.exponentialRampToValueAtTime(0.001, audioContext.currentTime + duration / 1000);
                
                oscillator.start(audioContext.currentTime);
                oscillator.stop(audioContext.currentTime + duration / 1000);
                
                console.log(`🔊 Playing beep: ${frequency}Hz for ${duration}ms`);
                return true;
            } catch (e) {
                console.log('❌ Beep failed:', e);
                return false;
            }
        }
        
                 // Alternative method using Speech Synthesis (works without user interaction)
         let bestVoice = null;
         let voicesLoaded = false;
         
         function loadBestEnglishVoice() {
             if (!('speechSynthesis' in window)) return;
             
             const voices = speechSynthesis.getVoices();
             if (voices.length === 0) return;
             
             // Priority order for best English voices
             const preferredVoices = [
                 'Google US English',
                 'Microsoft Zira Desktop',
                 'Microsoft David Desktop',
                 'Alex',
                 'Samantha',
                 'Karen',
                 'Daniel'
             ];
             
             // Try to find preferred voices first
             for (const preferred of preferredVoices) {
                 const voice = voices.find(v => v.name.includes(preferred));
                 if (voice) {
                     bestVoice = voice;
                     console.log(`✅ Selected voice: ${voice.name} (${voice.lang})`);
                     return;
                 }
             }
             
             // Fallback: find any clear English voice
             const englishVoices = voices.filter(v => 
                 v.lang.startsWith('en') && 
                 (v.name.includes('English') || v.name.includes('US') || v.name.includes('UK'))
             );
             
             if (englishVoices.length > 0) {
                 bestVoice = englishVoices[0];
                 console.log(`✅ Selected fallback voice: ${bestVoice.name} (${bestVoice.lang})`);
             } else if (voices.length > 0) {
                 bestVoice = voices[0];
                 console.log(`⚠️ Using default voice: ${bestVoice.name} (${bestVoice.lang})`);
             }
             
             voicesLoaded = true;
         }
         
         function playTextToSpeech(text = "Temperature Alert!") {
             if (!('speechSynthesis' in window)) return false;
             
             // Load voices if not already loaded
             if (!voicesLoaded) {
                 loadBestEnglishVoice();
                 // If still no voices, wait a bit and try again
                 if (!voicesLoaded) {
                     setTimeout(() => {
                         loadBestEnglishVoice();
                         if (voicesLoaded) playTextToSpeech(text);
                     }, 100);
                     return false;
                 }
             }
             
             try {
                 // Clear any previous speech
                 speechSynthesis.cancel();
                 
                 // Create clearer, simpler message for better pronunciation
                 let clearText = text;
                 if (text.includes("Red Alert") || text.includes("critical")) {
                     clearText = "Warning! Temperature is too high!";
                 } else if (text.includes("test") || text.includes("ready")) {
                     clearText = "Audio test successful. Alert system is ready.";
                 } else if (text.includes("complete")) {
                     clearText = "Sound test complete. System ready.";
                 }
                 
                 const utterance = new SpeechSynthesisUtterance(clearText);
                 
                 // Use best available voice
                 if (bestVoice) {
                     utterance.voice = bestVoice;
                 }
                 
                 // Optimized settings for clarity
                 utterance.volume = 1.0;
                 utterance.rate = 0.8; // Slightly slower for clarity
                 utterance.pitch = 1.0; // Normal pitch for best clarity
                 
                 // Add pauses for better understanding
                 if (clearText.includes("Warning")) {
                     utterance.text = "Warning. Temperature is too high.";
                 }
                 
                 speechSynthesis.speak(utterance);
                 console.log(`🗣️ Speaking (${bestVoice ? bestVoice.name : 'default'}):`, clearText);
                 return true;
             } catch (e) {
                 console.log('❌ Text-to-speech failed:', e);
                 return false;
             }
         }
         
         // Load voices when available
         if ('speechSynthesis' in window) {
             speechSynthesis.onvoiceschanged = loadBestEnglishVoice;
             // Also try to load immediately
             setTimeout(loadBestEnglishVoice, 100);
         }
        
        // Try notification sound (system beep)
        function playSystemBeep() {
            try {
                // Try to create a data URL for a simple beep
                const audioElement = new Audio('data:audio/wav;base64,UklGRnoGAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQoGAACBhYqFbF1fdJivrJBhNjVgodDbq2EcBj+a2/LDciUFLIHO8tiJNwgZaLvt559NEAxQp+PwtmMcBjiR1/LMeSwFJHfH8N2QQAoUXrTp66hVFApGn+DyvmwhDy2JzfPYhT0F');
                audioElement.volume = 1.0;
                audioElement.play().then(() => {
                    console.log('🔔 System beep played');
                }).catch(e => {
                    console.log('❌ System beep failed:', e);
                });
                return true;
            } catch (e) {
                console.log('❌ System beep creation failed:', e);
                return false;
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
        
        let alertInterval = null;
        
        function playAlertSound() {
            if (!isAlertSounding) {
                isAlertSounding = true;
                console.log('🚨 RED ALERT! Temperature threshold exceeded!');
                
                // Multiple fallback methods for maximum compatibility
                
                // Method 1: Web Audio API beep (loudest)
                if (!playLoudBeep(1000, 800)) {
                    console.log('Web Audio failed, trying alternatives...');
                    
                                         // Method 2: Text-to-speech (works without user interaction)
                     if (!playTextToSpeech("Warning! Temperature critical!")) {
                        console.log('Text-to-speech failed, trying system beep...');
                        
                        // Method 3: System beep fallback
                        playSystemBeep();
                    }
                }
                
                // Start repeating alert every 3 seconds
                alertInterval = setInterval(() => {
                    if (isAlertSounding) {
                                                 if (!playLoudBeep(800, 600)) {
                             playTextToSpeech("Warning! Temperature too high!");
                         }
                    }
                }, 3000);
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
                
                // Stop speech synthesis
                if ('speechSynthesis' in window) {
                    speechSynthesis.cancel();
                }
                
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
            console.log('🔊 Testing all audio methods...');
            
            // Stop any current alert
            stopAlertSound();
            
            // First, enable audio (required by browsers)
            if (!enableAudio()) {
                alert('⚠️ Audio initialization failed. Your browser may not support audio.');
                return;
            }
            
            let testStep = 0;
            let anyTestSuccessful = false;
            const testSteps = [
                () => {
                    console.log('Test 1: Loud beep');
                    if (!playLoudBeep(1000, 1000)) {
                        console.log('❌ Loud beep failed');
                        return false;
                    }
                    anyTestSuccessful = true;
                    return true;
                },
                                 () => {
                     console.log('Test 2: Text-to-speech');
                     const success = playTextToSpeech("Audio test successful. Alert system ready.");
                     if (success) anyTestSuccessful = true;
                     return success;
                 },
                () => {
                    console.log('Test 3: System beep');
                    const success = playSystemBeep();
                    if (success) anyTestSuccessful = true;
                    return success;
                },
                () => {
                    console.log('Test 4: High frequency beep');
                    const success = playLoudBeep(1500, 500);
                    if (success) anyTestSuccessful = true;
                    return success;
                },
                () => {
                    console.log('Test 5: Low frequency beep');
                    const success = playLoudBeep(600, 500);
                    if (success) anyTestSuccessful = true;
                    return success;
                }
            ];
            
            function runNextTest() {
                if (testStep < testSteps.length) {
                    const success = testSteps[testStep]();
                    if (success) {
                        console.log(`✅ Test ${testStep + 1} successful`);
                    } else {
                        console.log(`❌ Test ${testStep + 1} failed`);
                    }
                    testStep++;
                    setTimeout(runNextTest, 1200); // Wait between tests
                } else {
                    console.log('🔇 All sound tests completed!');
                    
                    // Show green "Audio Ready" status if any test was successful
                    if (anyTestSuccessful) {
                        document.getElementById('audio-test-section').style.display = 'none';
                        document.getElementById('audio-ready-section').style.display = 'block';
                        console.log('✅ Audio system is ready for alerts!');
                        
                                             if (playTextToSpeech) {
                         playTextToSpeech("Sound test complete. System ready.");
                     }
                    } else {
                        console.log('❌ All audio tests failed. Alerts may not work properly.');
                        alert('⚠️ All audio tests failed. Your browser may not support audio alerts.');
                    }
                }
            }
            
            // Start the test sequence
            runNextTest();
        }
        
        function testVoiceOnly() {
            console.log('🗣️ Testing voice only...');
            enableAudio();
            
            if (playTextToSpeech("This is a voice test. Can you understand me clearly?")) {
                console.log('✅ Voice test started');
            } else {
                alert('❌ Voice test failed. Text-to-speech not available.');
            }
        }
        
        function showAvailableVoices() {
            if (!('speechSynthesis' in window)) {
                alert('❌ Text-to-speech not supported in this browser.');
                return;
            }
            
            loadBestEnglishVoice();
            const voices = speechSynthesis.getVoices();
            
            if (voices.length === 0) {
                alert('⚠️ No voices available. Try refreshing the page.');
                return;
            }
            
            console.log('📋 Available Voices:');
            voices.forEach((voice, index) => {
                const selected = (bestVoice && voice.name === bestVoice.name) ? ' ✅ SELECTED' : '';
                console.log(`${index + 1}. ${voice.name} (${voice.lang})${selected}`);
            });
            
            const englishVoices = voices.filter(v => v.lang.startsWith('en'));
            const selectedVoice = bestVoice ? `${bestVoice.name} (${bestVoice.lang})` : 'Default';
            
            alert(`🗣️ Voice Information:\n\n` +
                  `Currently Selected: ${selectedVoice}\n` +
                  `Total Voices: ${voices.length}\n` +
                  `English Voices: ${englishVoices.length}\n\n` +
                  `Check console (F12) for full voice list.`);
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
                
                // Update system status
                const memStatus = document.getElementById('memory-status');
                const storageStatus = document.getElementById('storage-status');
                const emergencyStatus = document.getElementById('emergency-status');
                
                const memPercent = current.memory_usage_percent || 0;
                const freeHeap = current.free_heap_kb || 0;
                const emergency = current.emergency_mode || false;
                const persistent = current.persistent_storage || false;
                
                memStatus.innerHTML = `Memory: ${memPercent}% (${freeHeap}KB free)`;
                memStatus.style.color = memPercent > 90 ? '#d32f2f' : memPercent > 80 ? '#ff9800' : '#4caf50';
                
                storageStatus.innerHTML = `Storage: ${persistent ? '✅ Active' : '❌ Failed'}`;
                storageStatus.style.color = persistent ? '#4caf50' : '#d32f2f';
                
                emergencyStatus.innerHTML = `Mode: ${emergency ? '🚨 Emergency' : '✅ Normal'}`;
                emergencyStatus.style.color = emergency ? '#d32f2f' : '#4caf50';
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
  
  // Initialize SPIFFS for persistent data storage
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS initialization failed - no persistent storage available");
  } else {
    Serial.println("✅ SPIFFS initialized - persistent storage ready");
    
    // Load previous data and configuration
    loadFromPersistentStorage();
    
    // Initialize memory tracking
    lastMemoryCheck = millis();
    lastSPIFFSSave = getCurrentTimestamp();
    
    Serial.printf("💾 Memory usage at startup: %d%% (%d KB free)\n", 
                  getMemoryUsagePercent(), ESP.getFreeHeap() / 1024);
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
  server.on("/api/save", HTTP_POST, handleSaveData);
  
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
  
  // Check memory usage periodically (every 30 seconds)
  if (millis() - lastMemoryCheck >= 30000) {
    lastMemoryCheck = millis();
    checkMemoryUsage();
    
    // Log memory status periodically
    Serial.printf("📊 Memory: %d%% used (%d KB free), Buffers: %d detailed + %d aggregated\n",
                  getMemoryUsagePercent(), ESP.getFreeHeap() / 1024, 
                  detailedBuffer.size(), aggregatedBuffer.size());
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