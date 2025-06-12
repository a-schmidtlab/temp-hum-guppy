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
  
  // Check for temperature and humidity alerts
  checkTemperatureAlert(t);
  checkHumidityAlert(h);
  
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
  doc["humidity_alert_threshold"] = humidityAlertThreshold;
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
  
  if (!error) {
    if (doc.containsKey("alert_threshold")) {
      alertThreshold = doc["alert_threshold"];
      Serial.printf("📂 Loaded temperature alert threshold: %.1f°C from persistent storage\n", alertThreshold);
    }
    if (doc.containsKey("humidity_alert_threshold")) {
      humidityAlertThreshold = doc["humidity_alert_threshold"];
      Serial.printf("📂 Loaded humidity alert threshold: %.1f%% from persistent storage\n", humidityAlertThreshold);
    }
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
  }
  // Remove the automatic clearing - alert stays active until acknowledged
  // Only check for new alerts if no alert is currently active
}

void checkHumidityAlert(float humidity) {
  if (humidity > humidityAlertThreshold) {
    if (!humidityAlertActive) {
      // New humidity alert triggered
      humidityAlertActive = true;
      humidityAlertAcknowledged = false;
      Serial.printf("HUMIDITY ALERT! Current: %.1f%%, Threshold: %.1f%%\n", humidity, humidityAlertThreshold);
    }
  }
  // Remove the automatic clearing - alert stays active until acknowledged
  // Only check for new alerts if no alert is currently active
}

void handleSetAlert(AsyncWebServerRequest *req) {
  if (req->hasParam("threshold")) {
    float newThreshold = req->getParam("threshold")->value().toFloat();
    if (newThreshold > 0 && newThreshold < 100) {
      alertThreshold = newThreshold;
      Serial.printf("Temperature alert threshold set to: %.1f°C\n", alertThreshold);
      saveConfigToPersistentStorage(); // Save config immediately
      req->send(200, "application/json", "{\"status\":\"ok\",\"threshold\":" + String(alertThreshold) + "}");
    } else {
      req->send(400, "application/json", "{\"error\":\"Invalid threshold range (0-100°C)\"}");
    }
  } else {
    req->send(400, "application/json", "{\"error\":\"Missing threshold parameter\"}");
  }
}

void handleSetHumidityAlert(AsyncWebServerRequest *req) {
  if (req->hasParam("threshold")) {
    float newThreshold = req->getParam("threshold")->value().toFloat();
    if (newThreshold > 0 && newThreshold <= 100) {
      humidityAlertThreshold = newThreshold;
      Serial.printf("Humidity alert threshold set to: %.1f%%\n", humidityAlertThreshold);
      saveConfigToPersistentStorage(); // Save config immediately
      req->send(200, "application/json", "{\"status\":\"ok\",\"threshold\":" + String(humidityAlertThreshold) + "}");
    } else {
      req->send(400, "application/json", "{\"error\":\"Invalid threshold range (0-100%)\"}");
    }
  } else {
    req->send(400, "application/json", "{\"error\":\"Missing threshold parameter\"}");
  }
}

void handleAckAlert(AsyncWebServerRequest *req) {
  if (alertActive) {
    alertActive = false;
    alertAcknowledged = true;
    Serial.println("Temperature alert acknowledged by user - alert cleared");
    req->send(200, "application/json", "{\"status\":\"acknowledged\"}");
  } else {
    req->send(200, "application/json", "{\"status\":\"no_active_alert\"}");
  }
}

void handleAckHumidityAlert(AsyncWebServerRequest *req) {
  if (humidityAlertActive) {
    humidityAlertActive = false;
    humidityAlertAcknowledged = true;
    Serial.println("Humidity alert acknowledged by user - alert cleared");
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

void handleGetHumidityAlert(AsyncWebServerRequest *req) {
  StaticJsonDocument<256> doc;
  doc["threshold"] = humidityAlertThreshold;
  doc["active"] = humidityAlertActive;
  doc["acknowledged"] = humidityAlertAcknowledged;
  doc["needs_attention"] = (humidityAlertActive && !humidityAlertAcknowledged);
  
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
  doc["uptime_seconds"] = millis() / 1000;  // Add actual uptime in seconds since boot
  
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
  // Ultra-compact HTML - all functionality preserved but much smaller for ESP32 memory
  String html = F("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>REUTERS UW-CAM1 Environmental Monitor</title><script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script><style>*{margin:0;padding:0;box-sizing:border-box}body{font-family:Arial,sans-serif;background:#1a1a1a;color:#e0e0e0;line-height:1.4}.header{background:linear-gradient(135deg,#2c3e50,#34495e);padding:8px 16px;border-bottom:2px solid #3498db;display:flex;justify-content:space-between;align-items:center}.header h1{font-size:16px;color:#ecf0f1;margin:0}.header .timestamp{font-size:12px;color:#bdc3c7}.container{padding:12px}.status-grid{display:grid;grid-template-columns:1fr 1fr 1fr 1fr;gap:8px;margin-bottom:12px}.status-panel{background:#2c3e50;border:1px solid #34495e;border-radius:4px;padding:8px;text-align:center;min-height:70px;display:flex;flex-direction:column;justify-content:center}.status-panel.alert{border-color:#e74c3c;background:#c0392b;animation:alertBlink 1s infinite}@keyframes alertBlink{0%,100%{opacity:1}50%{opacity:0.7}}.status-value{font-size:24px;font-weight:bold;color:#ecf0f1}.status-label{font-size:11px;color:#bdc3c7;margin-top:2px}.status-unit{font-size:14px;color:#95a5a6}.monitoring-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px}.control-section{background:#34495e;border:1px solid #5d6d7e;border-radius:4px;margin-bottom:8px;overflow:hidden}.control-header{background:#2c3e50;padding:6px 12px;border-bottom:1px solid #5d6d7e;font-size:12px;font-weight:bold;color:#ecf0f1}.control-content{padding:8px 12px}.control-row{display:flex;align-items:center;gap:8px;margin-bottom:6px;font-size:12px}.control-row:last-child{margin-bottom:0}input[type=\"number\"]{width:60px;padding:4px 6px;background:#2c3e50;border:1px solid #5d6d7e;border-radius:3px;color:#ecf0f1;font-size:12px}select{padding:4px 6px;background:#2c3e50;border:1px solid #5d6d7e;border-radius:3px;color:#ecf0f1;font-size:12px}button{padding:4px 8px;background:#3498db;border:none;border-radius:3px;color:white;font-size:11px;cursor:pointer}button:hover{background:#2980b9}button.danger{background:#e74c3c}button.warning{background:#f39c12}.charts-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px}.chart-panel{background:#34495e;border:1px solid #5d6d7e;border-radius:4px;padding:8px;height:250px}.chart-title{font-size:12px;font-weight:bold;color:#ecf0f1;margin-bottom:8px;text-align:center}canvas{max-height:220px}.system-status{display:flex;gap:12px;font-size:10px;color:#95a5a6;margin-top:8px}.status-indicator{display:flex;align-items:center;gap:4px}.status-led{width:8px;height:8px;border-radius:50%;background:#27ae60}.status-led.warning{background:#f39c12}.status-led.error{background:#e74c3c}@media (max-width:768px){.status-grid{grid-template-columns:1fr 1fr}.monitoring-grid{grid-template-columns:1fr}.charts-grid{grid-template-columns:1fr}}</style></head><body><div class=\"header\"><h1>REUTERS UW-CAM1 -- ENVIRONMENTAL MONITORING SYSTEM</h1><div class=\"timestamp\" id=\"t\">--:--:--</div></div><div class=\"container\"><div class=\"status-grid\"><div class=\"status-panel\" id=\"tp\"><div class=\"status-value\" id=\"tv\">--</div><div class=\"status-label\">TEMPERATURE <span class=\"status-unit\">°C</span></div></div><div class=\"status-panel\" id=\"hp\"><div class=\"status-value\" id=\"hv\">--</div><div class=\"status-label\">HUMIDITY <span class=\"status-unit\">%</span></div></div><div class=\"status-panel\"><div class=\"status-value\" id=\"mv\">--</div><div class=\"status-label\">MEMORY <span class=\"status-unit\">%</span></div></div><div class=\"status-panel\"><div class=\"status-value\" id=\"uv\">--</div><div class=\"status-label\">UPTIME</div></div></div><div class=\"monitoring-grid\"><div class=\"control-section\"><div class=\"control-header\">TEMPERATURE MONITORING</div><div class=\"control-content\"><div class=\"control-row\"><span>Threshold:</span><input type=\"number\" id=\"at\" min=\"0\" max=\"100\" step=\"0.1\" value=\"40.0\"><span>°C</span><button onclick=\"setTemp()\">SET</button><span id=\"ts\">NORMAL</span><button id=\"ab\" onclick=\"ackTemp()\" class=\"danger\" style=\"display:none;\">ACK</button></div></div></div><div class=\"control-section\"><div class=\"control-header\">HUMIDITY MONITORING</div><div class=\"control-content\"><div class=\"control-row\"><span>Threshold:</span><input type=\"number\" id=\"ht\" min=\"0\" max=\"100\" step=\"0.1\" value=\"90.0\"><span>%</span><button onclick=\"setHum()\">SET</button><span id=\"hs\">NORMAL</span><button id=\"hb\" onclick=\"ackHum()\" class=\"danger\" style=\"display:none;\">ACK</button></div></div></div></div><div class=\"charts-grid\"><div class=\"chart-panel\"><div class=\"chart-title\">TEMPERATURE TREND</div><canvas id=\"tc\"></canvas></div><div class=\"chart-panel\"><div class=\"chart-title\">HUMIDITY TREND</div><canvas id=\"hc\"></canvas></div></div><div class=\"control-section\"><div class=\"control-header\">DATA VIEW</div><div class=\"control-content\"><div class=\"control-row\"><span>Range:</span><select id=\"rs\"><option value=\"detailed\">30s intervals (30min)</option><option value=\"aggregated\">5min intervals (24h)</option><option value=\"all\">All data</option></select><span id=\"di\">--</span></div></div></div><div class=\"control-section\"><div class=\"control-header\">AUDIO ALERT SYSTEM</div><div class=\"control-content\"><div class=\"control-row\"><button onclick=\"testAudio()\" class=\"warning\">TEST AUDIO</button><span id=\"as\">CLICK TEST TO ENABLE</span></div></div></div><div class=\"system-status\"><div class=\"status-indicator\"><div class=\"status-led\" id=\"sl\"></div><span id=\"ss\">STORAGE: --</span></div><div class=\"status-indicator\"><div class=\"status-led\"></div><span>NETWORK: CONNECTED</span></div><div class=\"status-indicator\"><div class=\"status-led\" id=\"el\"></div><span id=\"es\">MODE: --</span></div></div></div><script>let tC,hC,ctx,audio=false,alert=false,timer;async function get(u){try{return await(await fetch(u)).json()}catch{return null}}async function post(u,d){try{const p=new URLSearchParams(d);return await(await fetch(u+'?'+p.toString(),{method:'POST'})).json()}catch{return null}}function beep(f=1000,d=500){try{if(!ctx)ctx=new(window.AudioContext||window.webkitAudioContext)();if(ctx.state==='suspended')ctx.resume();const o=ctx.createOscillator(),g=ctx.createGain();o.connect(g);g.connect(ctx.destination);o.type='square';o.frequency.value=f;g.gain.setValueAtTime(0,ctx.currentTime);g.gain.linearRampToValueAtTime(0.3,ctx.currentTime+0.01);g.gain.exponentialRampToValueAtTime(0.001,ctx.currentTime+d/1000);o.start();o.stop(ctx.currentTime+d/1000);return true}catch{return false}}function speak(t){try{speechSynthesis.cancel();const u=new SpeechSynthesisUtterance(t);u.volume=1;speechSynthesis.speak(u);return true}catch{return false}}function startAlert(t){if(!alert){alert=true;let msg=t===\"humidity\"?\"Humidity alert\":\"Temperature alert\";if(timer){clearInterval(timer);timer=null}timer=setInterval(()=>{if(alert){if(!beep(1200,400))speak(msg)}},1000)}}function stopAlert(){if(alert){alert=false;if(timer){clearInterval(timer);timer=null}if(speechSynthesis)speechSynthesis.cancel();setTimeout(()=>{beep(800,200);setTimeout(()=>beep(600,200),250)},100)}}async function updateAlerts(){const ta=await get('/api/alert/get');if(ta){document.getElementById('at').value=ta.threshold.toFixed(1);const s=document.getElementById('ts'),p=document.getElementById('tp'),b=document.getElementById('ab');if(ta.needs_attention){s.textContent='CRITICAL - CLICK ACK!';s.style.color='#e74c3c';s.style.fontWeight='bold';s.style.animation='alertBlink 0.5s infinite';p.classList.add('alert');b.style.display='inline-block';b.style.animation='alertBlink 0.5s infinite';if(!alert)startAlert(\"temperature\")}else if(ta.active&&ta.acknowledged){s.textContent='HIGH (ACK)';s.style.color='#f39c12';p.classList.add('alert');b.style.display='none';stopAlert()}else{s.textContent='NORMAL';s.style.color='#27ae60';p.classList.remove('alert');b.style.display='none';stopAlert()}}const ha=await get('/api/humidity-alert/get');if(ha){document.getElementById('ht').value=ha.threshold.toFixed(1);const s=document.getElementById('hs'),p=document.getElementById('hp'),b=document.getElementById('hb');if(ha.needs_attention){s.textContent='CRITICAL';s.style.color='#e74c3c';p.classList.add('alert');b.style.display='inline-block';if(!alert)startAlert(\"humidity\")}else if(ha.active&&ha.acknowledged){s.textContent='HIGH (ACK)';s.style.color='#f39c12';p.classList.add('alert');b.style.display='none';stopAlert()}else{s.textContent='NORMAL';s.style.color='#27ae60';p.classList.remove('alert');b.style.display='none';stopAlert()}}}async function updateCurrent(){const c=await get('/api/current');if(c&&!c.error){document.getElementById('tv').textContent=c.t.toFixed(1);document.getElementById('hv').textContent=c.h.toFixed(0);document.getElementById('mv').textContent=c.memory_usage_percent||'--';const us=c.uptime_seconds||0,uh=Math.floor(us/3600),um=Math.floor((us%3600)/60);document.getElementById('uv').textContent=uh>0?uh+'h'+(um>0?um+'m':''):um+'m';document.getElementById('t').textContent=new Date().toLocaleTimeString();const ps=c.persistent_storage||false,em=c.emergency_mode||false;const sl=document.getElementById('sl'),ss=document.getElementById('ss');if(ps){sl.className='status-led';ss.textContent='STORAGE: ACTIVE'}else{sl.className='status-led error';ss.textContent='STORAGE: FAILED'}const el=document.getElementById('el'),es=document.getElementById('es');if(em){el.className='status-led error';es.textContent='MODE: EMERGENCY'}else{el.className='status-led';es.textContent='MODE: NORMAL'}document.getElementById('di').textContent=`${c.detailed_samples}/${c.aggregated_samples} samples`}updateAlerts()}async function updateCharts(){const r=document.getElementById('rs').value,h=await get('/api/history?range='+r);if(!h||!h.data)return;const l=h.data.map(i=>{if(i.ts>1000000000){const d=new Date(i.ts*1000);return r==='detailed'?d.toLocaleTimeString():d.toLocaleString()}else{return`+${i.ts}s`}}),t=h.data.map(i=>i.t),hum=h.data.map(i=>i.h);if(tC)tC.destroy();if(hC)hC.destroy();tC=new Chart(document.getElementById('tc'),{type:'line',data:{labels:l,datasets:[{label:'Temperature (°C)',data:t,borderColor:'rgb(255,99,132)',backgroundColor:'rgba(255,99,132,0.1)',tension:0.1}]},options:{responsive:true,maintainAspectRatio:true}});hC=new Chart(document.getElementById('hc'),{type:'line',data:{labels:l,datasets:[{label:'Humidity (%)',data:hum,borderColor:'rgb(54,162,235)',backgroundColor:'rgba(54,162,235,0.1)',tension:0.1}]},options:{responsive:true,maintainAspectRatio:true}})}async function setTemp(){const t=parseFloat(document.getElementById('at').value),r=await post('/api/alert/set',{threshold:t});if(r&&r.status==='ok')updateAlerts();else alert('Failed to set temperature threshold')}async function setHum(){const t=parseFloat(document.getElementById('ht').value),r=await post('/api/humidity-alert/set',{threshold:t});if(r&&r.status==='ok')updateAlerts();else alert('Failed to set humidity threshold')}async function ackTemp(){const r=await post('/api/alert/acknowledge',{});if(r){stopAlert();updateAlerts()}}async function ackHum(){const r=await post('/api/humidity-alert/acknowledge',{});if(r){stopAlert();updateAlerts()}}function testAudio(){if(!audio){if(beep(1000,800)){audio=true;document.getElementById('as').textContent='AUDIO READY';document.getElementById('as').style.color='#27ae60'}else{document.getElementById('as').textContent='AUDIO FAILED';document.getElementById('as').style.color='#e74c3c'}}else{startAlert(\"test\");setTimeout(stopAlert,3000)}}document.getElementById('rs').addEventListener('change',updateCharts);updateCurrent();updateCharts();setInterval(updateCurrent,30000);setInterval(()=>{const r=document.getElementById('rs').value;if(r==='detailed')updateCharts()},30000);setInterval(()=>{const r=document.getElementById('rs').value;if(r!=='detailed')updateCharts()},300000);</script></body></html>");
  
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
  server.on("/api/humidity-alert/get", HTTP_GET, handleGetHumidityAlert);
  server.on("/api/humidity-alert/set", HTTP_POST, handleSetHumidityAlert);
  server.on("/api/humidity-alert/acknowledge", HTTP_POST, handleAckHumidityAlert);
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