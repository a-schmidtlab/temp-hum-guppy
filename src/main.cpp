#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <DHT.h>
#include <ESPmDNS.h>        // Added for network discovery
#include <deque>
#include <vector>

// ---------- CONFIG ----------
constexpr bool USE_ETH   = true;     // set false if 3V3 < 3.25 V
const char *SSID    = "ULAM2.4";
const char *PASS    = "Diomedea42#";
const char *HOSTNAME = "tr-cam1-t-h-sensor";    // Device hostname for easy discovery
constexpr int   DHTPIN   = 4;        // GPIO4 for DHT11 data pin
constexpr int   DHTTYPE  = DHT11;
constexpr int   LED_PIN  = 2;        // Built-in LED for status indication
constexpr uint32_t SAMPLE_MS = 300000UL;   // 5-min period (300 seconds)
constexpr uint32_t NETWORK_CHECK_MS = 30000UL;  // Check network every 30 seconds
// -----------------------------

struct Reading { 
  uint32_t ts; 
  float t; 
  float h; 
};

std::deque<Reading> buf24h;
std::vector<Reading> buf7d, buf30d;

DHT dht(DHTPIN, DHTTYPE);
AsyncWebServer server(80);
uint32_t lastSample = 0;
uint32_t lastHourlySave = 0;
uint32_t lastNetworkCheck = 0;
bool isConnected = false;

// Forward declarations
void saveToPersistentStorage(float t, float h);

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
  Serial.println("🌐 NETWORK CONNECTION SUCCESS!");
  Serial.println("==================================================");
  
  if (ETH.linkUp()) {
    Serial.println("📡 Connection Type: Ethernet");
    Serial.printf("📍 IP Address: %s\n", ETH.localIP().toString().c_str());
    Serial.printf("🌐 Gateway: %s\n", ETH.gatewayIP().toString().c_str());
    Serial.printf("🔗 Subnet: %s\n", ETH.subnetMask().toString().c_str());
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.println("📡 Connection Type: WiFi");
    Serial.printf("📍 IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("🌐 Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("🔗 Subnet: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("📶 WiFi RSSI: %d dBm\n", WiFi.RSSI());
  }
  
  Serial.println("\n🎯 EASY ACCESS OPTIONS:");
  Serial.println("┌─────────────────────────────────────────┐");
  Serial.printf("│ 1. Browser:  http://%s.local       │\n", HOSTNAME);
  if (ETH.linkUp()) {
    Serial.printf("│ 2. Direct:   http://%-15s │\n", ETH.localIP().toString().c_str());
  } else {
    Serial.printf("│ 2. Direct:   http://%-15s │\n", WiFi.localIP().toString().c_str());
  }
  Serial.printf("│ 3. Hostname: %s                │\n", HOSTNAME);
  Serial.println("└─────────────────────────────────────────┘");
  Serial.println("\n💡 TIP: Use option 1 on most networks!");
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
      Serial.println("✅ Network reconnected");
      blinkStatusLED(3, 100);  // 3 quick blinks = connected
      printNetworkInfo();
    } else {
      Serial.println("❌ Network disconnected");
      blinkStatusLED(1, 1000); // 1 long blink = disconnected
    }
  }
}

// Helper functions
void addReading(float t, float h) {
  if (isnan(t) || isnan(h)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }
  
  uint32_t now = millis() / 1000;
  buf24h.push_back({now, t, h});
  
  // Keep only 24h of data (288 samples at 5-min intervals)
  if (buf24h.size() > 288) {
    buf24h.pop_front();
  }
  
  Serial.printf("Reading: %.1f°C, %.0f%% RH\n", t, h);
  
  // Save to persistent storage every hour
  if ((now - lastHourlySave) >= 3600) {
    saveToPersistentStorage(t, h);
    lastHourlySave = now;
  }
}

void saveToPersistentStorage(float t, float h) {
  // Add to 7-day buffer
  uint32_t now = millis() / 1000;
  buf7d.push_back({now, t, h});
  
  // Keep only 7 days of data (2016 samples)
  if (buf7d.size() > 2016) {
    buf7d.erase(buf7d.begin());
  }
  
  // Add to 30-day buffer  
  buf30d.push_back({now, t, h});
  
  // Keep only 30 days of data (8640 samples)
  if (buf30d.size() > 8640) {
    buf30d.erase(buf30d.begin());
  }
  
  Serial.println("Data saved to persistent storage");
}

void handleCurrent(AsyncWebServerRequest *req) {
  if (buf24h.empty()) {
    req->send(503, "application/json", "{\"error\":\"no data\"}");
    return;
  }
  
  StaticJsonDocument<128> doc;
  Reading last = buf24h.back();
  doc["t"] = last.t;
  doc["h"] = last.h;
  doc["timestamp"] = last.ts;
  
  String output;
  serializeJson(doc, output);
  req->send(200, "application/json", output);
}

void handleHistory(AsyncWebServerRequest *req) {
  String range = "24h";
  if (req->hasParam("range")) {
    range = req->getParam("range")->value();
  }
  
  DynamicJsonDocument doc(16384); // 16KB for JSON response
  JsonArray data = doc.createNestedArray("data");
  
  if (range == "24h") {
    for (const auto& reading : buf24h) {
      JsonObject obj = data.createNestedObject();
      obj["ts"] = reading.ts;
      obj["t"] = reading.t;
      obj["h"] = reading.h;
    }
  } else if (range == "7d") {
    for (const auto& reading : buf7d) {
      JsonObject obj = data.createNestedObject();
      obj["ts"] = reading.ts;
      obj["t"] = reading.t;
      obj["h"] = reading.h;
    }
  } else if (range == "30d") {
    for (const auto& reading : buf30d) {
      JsonObject obj = data.createNestedObject();
      obj["ts"] = reading.ts;
      obj["t"] = reading.t;
      obj["h"] = reading.h;
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
        .charts { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; margin-top: 20px; }
        .chart-container { background: #fafafa; padding: 15px; border-radius: 8px; }
        select { padding: 8px; font-size: 16px; margin-bottom: 15px; }
        canvas { max-height: 300px; }
        h1 { color: #333; text-align: center; }
        h2 { color: #666; margin: 0; }
        .loading { text-align: center; color: #666; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🌡️ Temperature & Humidity Monitor</h1>
        
        <div class="current">
            <h2 id="current-data" class="loading">Loading current data...</h2>
        </div>
        
        <div>
            <label for="range-select">Time Range:</label>
            <select id="range-select">
                <option value="24h">Last 24 Hours</option>
                <option value="7d">Last 7 Days</option>
                <option value="30d">Last 30 Days</option>
            </select>
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

    <script>
        let tempChart, humidityChart;
        
        async function fetchJson(url) {
            try {
                const response = await fetch(url);
                return await response.json();
            } catch (error) {
                console.error('Fetch error:', error);
                return null;
            }
        }
        
        async function updateCurrent() {
            const current = await fetchJson('/api/current');
            if (current && !current.error) {
                document.getElementById('current-data').innerHTML = 
                    `Current: <strong>${current.t.toFixed(1)}°C</strong> | <strong>${current.h.toFixed(0)}%</strong> RH`;
            }
        }
        
        async function updateCharts() {
            const range = document.getElementById('range-select').value;
            const history = await fetchJson('/api/history?range=' + range);
            
            if (!history || !history.data) return;
            
            const labels = history.data.map(item => new Date(item.ts * 1000).toLocaleString());
            const temps = history.data.map(item => item.t);
            const humidity = history.data.map(item => item.h);
            
            // Destroy existing charts
            if (tempChart) tempChart.destroy();
            if (humidityChart) humidityChart.destroy();
            
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
                            beginAtZero: false
                        }
                    }
                }
            });
            
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
                            beginAtZero: true,
                            max: 100
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
        
        // Update current data every 30 seconds
        setInterval(updateCurrent, 30000);
        
        // Update charts every 5 minutes
        setInterval(updateCharts, 300000);
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
  Serial.println("🔗 Connecting to network...");
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
      Serial.println("\n✅ Ethernet connected!");
      isConnected = true;
      setupNetworkDiscovery();
      printNetworkInfo();
    } else {
      Serial.println("\n❌ Ethernet failed, falling back to WiFi");
      WiFi.setHostname(HOSTNAME);
      WiFi.begin(SSID, PASS);
      while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.print(".");
      }
      Serial.println("\n✅ WiFi connected!");
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
    Serial.println("\n✅ WiFi connected!");
    isConnected = true;
    setupNetworkDiscovery();
    printNetworkInfo();
  }
  
  // Setup web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/current", HTTP_GET, handleCurrent);
  server.on("/api/history", HTTP_GET, handleHistory);
  
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
    
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    
    addReading(temperature, humidity);
    
    // Blink LED to show activity
    if (isConnected) {
      blinkStatusLED(1, 50); // Quick blink on successful reading
    }
  }
  
  delay(100);  // Small delay to prevent watchdog issues
} 