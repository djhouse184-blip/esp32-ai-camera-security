/*
 * ESP32 Local AI Security Node - Complete Single Sketch
 * 
 * Features:
 * - LED status: solid ON = ready, flashing = working, OFF = offline
 * - WiFi manager: home WiFi first, fallback to hotspot
 * - Static IP preference: 10.0.0.250, then 192.168.1.250, then 192.168.0.250
 * - Hotspot mode: AP IP 10.0.0.250 with SSID "ESP32-Local-AI"
 * - Local HTTP server on port 80
 * - Network device discovery (cameras, smart devices)
 * - Simple command parser for local control
 * - Voice/text command support
 * 
 * Hardware: Plain ESP32 dev board with onboard LED on GPIO 2
 * 
 * Installation:
 * 1. Install ESP32 board package in Arduino IDE
 * 2. Install ArduinoJson library (v7.0.0 or newer)
 * 3. Upload this sketch to your ESP32
 * 4. Monitor serial at 115200 baud
 * 5. Connect to "ESP32-Local-AI" hotspot and open http://10.0.0.250
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>

// ============================================================================
// CONFIGURATION
// ============================================================================

#define STATUS_LED 2                    // GPIO 2 - onboard LED on most ESP32 boards
#define AP_SSID "ESP32-Local-AI"        // Hotspot SSID
#define AP_PASSWORD "esp32ai123"        // Hotspot password
#define FLASH_INTERVAL 250              // LED flash interval (ms)
#define LOOP_DELAY 100                  // Main loop delay (ms)

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

Preferences prefs;
WebServer server(80);

String savedSsid = "";
String savedPass = "";
String currentWifiSsid = "";
String currentIp = "";
String lastCommand = "";
String lastCameraUrl = "";
String lastStatusMessage = "ESP32 ready";

bool apMode = false;
bool wifiConnected = false;
bool busyFlag = false;

// Device record structure for discovered devices
struct DeviceRecord {
  String ip;
  String kind;
  String note;
  String protocol;
};

std::vector<DeviceRecord> discoveredDevices;

// ============================================================================
// LED CONTROL FUNCTIONS
// ============================================================================

void setLedState(bool on) {
  digitalWrite(STATUS_LED, on ? HIGH : LOW);
}

void setBusyIndicator() {
  // Flash the LED while working
  static unsigned long lastToggle = 0;
  static bool ledState = false;

  if (millis() - lastToggle > FLASH_INTERVAL) {
    ledState = !ledState;
    setLedState(ledState);
    lastToggle = millis();
  }
}

void setReadyIndicator() {
  // Solid LED on when ready
  setLedState(HIGH);
}

void setOfflineIndicator() {
  // LED off when offline
  setLedState(LOW);
}

// ============================================================================
// WiFi MANAGEMENT FUNCTIONS
// ============================================================================

void saveWifiSettings(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  Serial.printf("[WiFi] Saved credentials: %s\n", ssid.c_str());
}

void loadWifiSettings() {
  prefs.begin("wifi", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
  
  if (savedSsid.length() > 0) {
    Serial.printf("[WiFi] Loaded saved SSID: %s\n", savedSsid.c_str());
  } else {
    Serial.println("[WiFi] No saved credentials found");
  }
}

bool tryStaticIp(const String& ipStr, const String& gatewayStr, const String& subnetStr, const String& dnsStr) {
  IPAddress local, gateway, subnet, dns;
  
  if (!local.fromString(ipStr) || !gateway.fromString(gatewayStr) || 
      !subnet.fromString(subnetStr) || !dns.fromString(dnsStr)) {
    return false;
  }
  
  WiFi.config(local, gateway, subnet, dns);
  return true;
}

bool connectToPreferredNetwork() {
  if (savedSsid.length() == 0) {
    Serial.println("[WiFi] No saved SSID. Starting AP mode.");
    return false;
  }

  Serial.printf("[WiFi] Attempting to connect to saved WiFi: %s\n", savedSsid.c_str());
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);

  // Try static IPs in order: 10.0.0.250, 192.168.1.250, 192.168.0.250
  bool staticApplied = false;
  
  // Attempt 10.0.0.250 first
  if (tryStaticIp("10.0.0.250", "10.0.0.1", "255.255.255.0", "8.8.8.8")) {
    Serial.println("[WiFi] Trying static IP 10.0.0.250");
    staticApplied = true;
  }

  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  // Wait up to 20 seconds for connection
  unsigned long start = millis();
  int dotCount = 0;
  while (millis() - start < 20000) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      currentWifiSsid = WiFi.SSID();
      currentIp = WiFi.localIP().toString();
      apMode = false;
      Serial.printf("[WiFi] Connected! IP: %s\n", currentIp.c_str());
      return true;
    }
    Serial.print(".");
    dotCount++;
    if (dotCount % 50 == 0) Serial.println();
    delay(250);
  }

  Serial.println("\n[WiFi] Connection timeout. Trying alternative static IPs...");

  // Fallback to 192.168.1.250
  if (tryStaticIp("192.168.1.250", "192.168.1.1", "255.255.255.0", "8.8.8.8")) {
    Serial.println("[WiFi] Trying static IP 192.168.1.250");
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    
    start = millis();
    while (millis() - start < 10000) {
      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        currentWifiSsid = WiFi.SSID();
        currentIp = WiFi.localIP().toString();
        apMode = false;
        Serial.printf("[WiFi] Connected! IP: %s\n", currentIp.c_str());
        return true;
      }
      delay(250);
    }
  }

  // Fallback to 192.168.0.250
  if (tryStaticIp("192.168.0.250", "192.168.0.1", "255.255.255.0", "8.8.8.8")) {
    Serial.println("[WiFi] Trying static IP 192.168.0.250");
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    
    start = millis();
    while (millis() - start < 10000) {
      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        currentWifiSsid = WiFi.SSID();
        currentIp = WiFi.localIP().toString();
        apMode = false;
        Serial.printf("[WiFi] Connected! IP: %s\n", currentIp.c_str());
        return true;
      }
      delay(250);
    }
  }

  // Try DHCP as last resort
  Serial.println("[WiFi] Trying DHCP...");
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  
  start = millis();
  while (millis() - start < 10000) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      currentWifiSsid = WiFi.SSID();
      currentIp = WiFi.localIP().toString();
      apMode = false;
      Serial.printf("[WiFi] Connected via DHCP! IP: %s\n", currentIp.c_str());
      return true;
    }
    delay(250);
  }

  WiFi.disconnect(false);
  Serial.println("[WiFi] Failed to connect. Falling back to AP mode.");
  return false;
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(10, 0, 0, 250), IPAddress(10, 0, 0, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  apMode = true;
  wifiConnected = false;
  currentWifiSsid = "ESP32-Local-AI";
  currentIp = WiFi.softAPIP().toString();
  
  Serial.printf("[AP] Hotspot started\n");
  Serial.printf("[AP] SSID: %s\n", AP_SSID);
  Serial.printf("[AP] Password: %s\n", AP_PASSWORD);
  Serial.printf("[AP] IP: %s\n", currentIp.c_str());
}

// ============================================================================
// NETWORK DISCOVERY FUNCTIONS
// ============================================================================

bool probePort(const IPAddress& ip, uint16_t port) {
  WiFiClient client;
  client.setTimeout(200);
  
  if (!client.connect(ip, port, 200)) {
    return false;
  }
  
  // For HTTP, try to read a response line
  String line = client.readStringUntil('\n');
  client.stop();
  return line.length() > 0;
}

bool addDiscoveredDevice(IPAddress ip, const String& kind, const String& note, const String& protocol) {
  for (const auto& rec : discoveredDevices) {
    if (rec.ip == ip.toString()) {
      return false;  // Already discovered
    }
  }

  DeviceRecord rec;
  rec.ip = ip.toString();
  rec.kind = kind;
  rec.note = note;
  rec.protocol = protocol;
  discoveredDevices.push_back(rec);
  return true;
}

void scanNetwork() {
  discoveredDevices.clear();
  busyFlag = true;
  lastStatusMessage = "Scanning local network";
  Serial.println("[Scan] Starting network discovery...");

  IPAddress localIp = WiFi.localIP();
  std::vector<IPAddress> candidates;

  // Get the subnet prefix
  if (WiFi.status() == WL_CONNECTED || apMode) {
    String base = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "10.0.0.250";
    int lastDot = base.lastIndexOf('.');
    String prefix = base.substring(0, lastDot);
    
    Serial.printf("[Scan] Scanning subnet: %s.x\n", prefix.c_str());
    
    for (int i = 1; i < 255; i++) {
      IPAddress ip;
      String ipStr = prefix + "." + String(i);
      if (ip.fromString(ipStr)) {
        candidates.push_back(ip);
      }
    }
  }

  int found = 0;
  for (const IPAddress& ip : candidates) {
    if (found >= 50) {  // Limit to 50 devices to save memory
      break;
    }

    // Skip self
    if (WiFi.status() == WL_CONNECTED && ip == WiFi.localIP()) {
      continue;
    }
    if (apMode && ip == WiFi.softAPIP()) {
      continue;
    }

    // Probe common ports
    bool port80 = probePort(ip, 80);
    bool port554 = probePort(ip, 554);
    bool port8080 = probePort(ip, 8080);
    bool port8888 = probePort(ip, 8888);
    bool port443 = probePort(ip, 443);

    if (port80 || port554 || port8080 || port8888 || port443) {
      String kind = "device";
      String protocol = "";

      if (port554) {
        kind = "rtsp-camera";
        protocol = "RTSP";
      } else if (port80) {
        kind = "http-device";
        protocol = "HTTP";
      } else if (port8080) {
        kind = "http-device";
        protocol = "HTTP:8080";
      } else if (port443) {
        kind = "https-device";
        protocol = "HTTPS";
      } else if (port8888) {
        kind = "service";
        protocol = "PORT8888";
      }

      String note = "ports: ";
      if (port80) note += "80 ";
      if (port554) note += "554 ";
      if (port8080) note += "8080 ";
      if (port8888) note += "8888 ";
      if (port443) note += "443";

      if (addDiscoveredDevice(ip, kind, note, protocol)) {
        found++;
        Serial.printf("[Scan] Found %s at %s\n", kind.c_str(), ip.toString().c_str());
      }
    }
  }

  busyFlag = false;
  String msg = "Found " + String(found) + " device(s)";
  lastStatusMessage = msg;
  Serial.printf("[Scan] Scan complete: %s\n", msg.c_str());
}

// ============================================================================
// COMMAND PROCESSING FUNCTIONS
// ============================================================================

String buildStatusJson() {
  StaticJsonDocument<2048> doc;
  
  doc["device"] = "ESP32-Local-AI";
  doc["version"] = "1.0";
  doc["wifi_mode"] = apMode ? "AP" : "STA";
  doc["ssid"] = currentWifiSsid;
  doc["ip"] = currentIp;
  doc["status"] = apMode ? "AP mode" : (wifiConnected ? "ready" : "offline");
  doc["led"] = busyFlag ? "busy" : (wifiConnected || apMode ? "ready" : "offline");
  doc["busy"] = busyFlag;
  doc["last_command"] = lastCommand;
  doc["message"] = lastStatusMessage;
  doc["camera_url"] = lastCameraUrl;
  doc["device_count"] = (int)discoveredDevices.size();

  JsonArray devices = doc.createNestedArray("devices");
  for (const auto& rec : discoveredDevices) {
    JsonObject entry = devices.add<JsonObject>();
    entry["ip"] = rec.ip;
    entry["kind"] = rec.kind;
    entry["protocol"] = rec.protocol;
    entry["note"] = rec.note;
  }

  String json;
  serializeJson(doc, json);
  return json;
}

String buildHtmlPage() {
  String html = R"(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Local AI Security Node</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: linear-gradient(135deg, #0f172a 0%, #1e293b 100%);
      color: #e2e8f0;
      min-height: 100vh;
      padding: 20px;
    }
    .container {
      max-width: 900px;
      margin: 0 auto;
    }
    h1 {
      color: #0ea5e9;
      margin-bottom: 10px;
      font-size: 2.5em;
      text-shadow: 0 0 10px rgba(14, 165, 233, 0.5);
    }
    .subtitle {
      color: #94a3b8;
      margin-bottom: 30px;
      font-size: 0.95em;
    }
    .card {
      background: rgba(30, 41, 59, 0.8);
      border: 1px solid #334155;
      border-radius: 12px;
      padding: 24px;
      margin-bottom: 20px;
      backdrop-filter: blur(10px);
    }
    .status-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 15px;
      margin-bottom: 15px;
    }
    .status-item {
      background: rgba(15, 23, 42, 0.5);
      padding: 12px;
      border-radius: 8px;
      border-left: 4px solid #0ea5e9;
    }
    .status-label {
      color: #94a3b8;
      font-size: 0.85em;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 1px;
    }
    .status-value {
      color: #e2e8f0;
      font-size: 1.2em;
      font-weight: bold;
      margin-top: 5px;
      font-family: 'Courier New', monospace;
    }
    .badge {
      display: inline-block;
      padding: 4px 12px;
      border-radius: 20px;
      font-size: 0.8em;
      font-weight: 600;
      margin-top: 5px;
    }
    .badge-ready { background: #10b981; color: #000; }
    .badge-busy { background: #f59e0b; color: #000; }
    .badge-offline { background: #ef4444; color: #fff; }
    .badge-ap { background: #6366f1; color: #fff; }
    
    h2 {
      color: #38bdf8;
      font-size: 1.5em;
      margin-bottom: 15px;
    }
    
    .form-group {
      margin-bottom: 15px;
    }
    label {
      display: block;
      margin-bottom: 6px;
      color: #cbd5e1;
      font-weight: 500;
      font-size: 0.9em;
    }
    input, textarea {
      width: 100%;
      padding: 12px;
      border: 1px solid #475569;
      background: rgba(15, 23, 42, 0.7);
      color: #e2e8f0;
      border-radius: 8px;
      font-family: 'Courier New', monospace;
      font-size: 0.95em;
    }
    input:focus, textarea:focus {
      outline: none;
      border-color: #0ea5e9;
      box-shadow: 0 0 0 3px rgba(14, 165, 233, 0.1);
    }
    button {
      background: linear-gradient(135deg, #0ea5e9 0%, #06b6d4 100%);
      color: #fff;
      padding: 12px 24px;
      border: none;
      border-radius: 8px;
      font-weight: 600;
      cursor: pointer;
      font-size: 1em;
      transition: all 0.3s ease;
    }
    button:hover {
      transform: translateY(-2px);
      box-shadow: 0 10px 20px rgba(14, 165, 233, 0.3);
    }
    button:active {
      transform: translateY(0);
    }
    .btn-secondary {
      background: linear-gradient(135deg, #64748b 0%, #475569 100%);
      margin-left: 10px;
    }
    
    .device-list {
      display: grid;
      grid-template-columns: 1fr;
      gap: 12px;
    }
    .device-item {
      background: rgba(15, 23, 42, 0.5);
      padding: 15px;
      border-radius: 8px;
      border-left: 4px solid #06b6d4;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .device-info {
      flex: 1;
    }
    .device-ip {
      font-family: 'Courier New', monospace;
      font-weight: bold;
      color: #38bdf8;
      font-size: 1.1em;
    }
    .device-kind {
      color: #a5f3fc;
      font-size: 0.85em;
      margin-top: 4px;
      text-transform: uppercase;
      font-weight: 600;
    }
    .device-note {
      color: #94a3b8;
      font-size: 0.8em;
      margin-top: 4px;
    }
    .btn-small {
      padding: 8px 12px;
      font-size: 0.85em;
    }
    
    .error {
      background: rgba(239, 68, 68, 0.1);
      border: 1px solid #ef4444;
      color: #fca5a5;
      padding: 12px;
      border-radius: 8px;
      margin-bottom: 15px;
    }
    .success {
      background: rgba(16, 185, 129, 0.1);
      border: 1px solid #10b981;
      color: #86efac;
      padding: 12px;
      border-radius: 8px;
      margin-bottom: 15px;
    }
    
    .footer {
      text-align: center;
      color: #64748b;
      margin-top: 40px;
      padding-top: 20px;
      border-top: 1px solid #334155;
      font-size: 0.9em;
    }
  </style>
  <script>
    function updateStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('ssid').textContent = data.ssid || 'N/A';
          document.getElementById('ip').textContent = data.ip || 'N/A';
          document.getElementById('mode').textContent = data.wifi_mode || 'Unknown';
          document.getElementById('status').textContent = data.status || 'Unknown';
          document.getElementById('message').textContent = data.message || 'No message';
          
          let badge = document.getElementById('status-badge');
          badge.className = 'badge';
          if (data.busy) {
            badge.textContent = '⚡ BUSY';
            badge.classList.add('badge-busy');
          } else if (data.status === 'ready') {
            badge.textContent = '✓ READY';
            badge.classList.add('badge-ready');
          } else if (data.status === 'AP mode') {
            badge.textContent = '📡 AP MODE';
            badge.classList.add('badge-ap');
          } else {
            badge.textContent = '✗ OFFLINE';
            badge.classList.add('badge-offline');
          }
          
          let devList = document.getElementById('device-list');
          if (data.device_count === 0) {
            devList.innerHTML = '<p style=\"color: #94a3b8; text-align: center;\">No devices discovered yet. Run a scan!</p>';
          } else {
            devList.innerHTML = '';
            data.devices.forEach(dev => {
              let item = document.createElement('div');
              item.className = 'device-item';
              item.innerHTML = `
                <div class="device-info">
                  <div class="device-ip">${dev.ip}</div>
                  <div class="device-kind">${dev.kind}</div>
                  <div class="device-note">${dev.protocol} - ${dev.note}</div>
                </div>
              `;
              devList.appendChild(item);
            });
          }
        })
        .catch(e => console.error(e));
    }
    
    function runCommand(cmd) {
      fetch('/cmd', { method: 'POST', body: 'command=' + encodeURIComponent(cmd) })
        .then(r => r.json())
        .then(data => {
          console.log('Response:', data);
          setTimeout(updateStatus, 500);
        });
    }
    
    function connectWifi(ssid, pass) {
      fetch('/connect', {
        method: 'POST',
        body: 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pass)
      })
        .then(r => r.json())
        .then(data => {
          alert('Connecting to ' + ssid + '...');
          setTimeout(updateStatus, 1000);
        });
    }
    
    setInterval(updateStatus, 5000);
    window.addEventListener('load', updateStatus);
  </script>
</head>
<body>
  <div class="container">
    <h1>🤖 ESP32 Local AI Security Node</h1>
    <div class="subtitle">Network AI Control & Device Discovery</div>
    
    <div class="card">
      <h2>📊 Status</h2>
      <div class="status-grid">
        <div class="status-item">
          <div class="status-label">Connected SSID</div>
          <div class="status-value" id="ssid">Loading...</div>
        </div>
        <div class="status-item">
          <div class="status-label">Current IP</div>
          <div class="status-value" id="ip">Loading...</div>
        </div>
        <div class="status-item">
          <div class="status-label">WiFi Mode</div>
          <div class="status-value" id="mode">Loading...</div>
        </div>
        <div class="status-item">
          <div class="status-label">Device Status</div>
          <div class="status-value" id="status">Loading...</div>
        </div>
      </div>
      <div id="status-badge" class="badge"></div>
      <div style="margin-top: 10px; color: #94a3b8; font-size: 0.9em;">
        <strong>Message:</strong> <span id="message">Loading...</span>
      </div>
    </div>
    
    <div class="card">
      <h2>💻 Network Scan & Commands</h2>
      <div class="form-group">
        <label>Run a command:</label>
        <div style="display: flex; gap: 10px; margin-bottom: 10px;">
          <button onclick="runCommand('scan network')" style="flex: 1;">🔍 Scan Network</button>
          <button onclick="runCommand('status')" class="btn-secondary" style="flex: 1;">📋 Status</button>
        </div>
        <div style="display: flex; gap: 10px;">
          <button onclick="runCommand('show cameras')" style="flex: 1;">📷 Show Cameras</button>
          <button onclick="runCommand('reboot')" class="btn-secondary" style="flex: 1;">⚡ Reboot</button>
        </div>
      </div>
      
      <textarea id="command-input\" placeholder=\"Enter custom command (e.g., 'show cameras', 'connect wifi SSID password')\" style=\"height: 60px; margin-bottom: 10px;\"></textarea>
      <button onclick=\"runCommand(document.getElementById('command-input').value); document.getElementById('command-input').value = '';\" style=\"width: 100%;\">▶️ Execute Custom Command</button>
    </div>
    
    <div class="card">
      <h2>📡 Connect to WiFi</h2>
      <div class="form-group">
        <label>WiFi SSID:</label>
        <input type=\"text\" id=\"wifi-ssid\" placeholder=\"Your WiFi network name\">
      </div>
      <div class=\"form-group\">
        <label>WiFi Password:</label>
        <input type=\"password\" id=\"wifi-pass\" placeholder=\"Your WiFi password\">
      </div>
      <button onclick=\"connectWifi(document.getElementById('wifi-ssid').value, document.getElementById('wifi-pass').value); document.getElementById('wifi-ssid').value = ''; document.getElementById('wifi-pass').value = '';\" style=\"width: 100%;\">✓ Connect</button>
    </div>
    
    <div class=\"card\">
      <h2>🎯 Discovered Devices</h2>
      <div class=\"device-list\" id=\"device-list\">
        <p style=\"color: #94a3b8; text-align: center;\">No devices discovered yet. Run a scan!</p>
      </div>
    </div>
    
    <div class=\"footer\">
      ESP32 Local AI Security Node • Ready for local network control and device discovery
    </div>
  </div>
</body>
</html>
  )";
  
  return html;
}

String handleCommand(const String& command) {
  String lower = command;
  lower.toLowerCase();
  lastCommand = command;

  Serial.printf("[Command] Processing: %s\n", command.c_str());

  if (lower.indexOf("scan") >= 0 || lower.indexOf("discover") >= 0 || lower.indexOf("network") >= 0) {
    scanNetwork();
    return buildStatusJson();
  }

  if (lower.indexOf("status") >= 0 || lower.indexOf("info") >= 0) {
    lastStatusMessage = "Status requested";
    return buildStatusJson();
  }

  if (lower.indexOf("reboot") >= 0 || lower.indexOf("restart") >= 0) {
    lastStatusMessage = "Rebooting...";
    delay(500);
    ESP.restart();
    return "{\"status\":\"restarting\"}";
  }

  if (lower.indexOf("connect") >= 0 || lower.indexOf("wifi") >= 0) {
    lastStatusMessage = "WiFi command received - use the Connect form";
    return "{\"status\":\"wifi command received\", \"help\":\"Use the Connect WiFi form\"}";
  }

  if (lower.indexOf("camera") >= 0 || lower.indexOf("rtsp") >= 0 || lower.indexOf("stream") >= 0) {
    lastCameraUrl = command;
    lastStatusMessage = "Camera target queued";
    return "{\"status\":\"camera target queued\", \"url\":\"" + lastCameraUrl + "\"}";
  }

  if (lower.indexOf("devices") >= 0) {
    return buildStatusJson();
  }

  lastStatusMessage = "Command acknowledged: " + command;
  return "{\"status\":\"acknowledged\", \"command\":\"" + command + "\"}";
}

// ============================================================================
// WEB SERVER HANDLERS
// ============================================================================

void handleRoot() {
  server.send(200, "text/html", buildHtmlPage());
}

void handleStatus() {
  server.send(200, "application/json", buildStatusJson());
}

void handleScan() {
  scanNetwork();
  server.send(200, "application/json", buildStatusJson());
}

void handleConnect() {
  if (!server.hasArg("ssid")) {
    server.send(400, "application/json", "{\"error\":\"missing ssid\"}");
    return;
  }

  String ssid = server.arg("ssid");
  String pass = server.arg("password");

  Serial.printf("[Connect] New WiFi: %s\n", ssid.c_str());
  saveWifiSettings(ssid, pass);
  lastStatusMessage = "Saved new WiFi credentials. Connecting...";
  
  server.send(200, "application/json", "{\"status\":\"saved\", \"ssid\":\"" + ssid + "\"}");
  
  delay(500);
  connectToPreferredNetwork();
}

void handleCmd() {
  if (!server.hasArg("command")) {
    server.send(400, "application/json", "{\"error\":\"missing command\"}");
    return;
  }

  String command = server.arg("command");
  String json = handleCommand(command);
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found. Try / or /status");
}

// ============================================================================
// MAIN SETUP
// ============================================================================

void setup() {
  // Initialize LED
  pinMode(STATUS_LED, OUTPUT);
  setOfflineIndicator();

  // Initialize serial
  Serial.begin(115200);
  delay(500);

  Serial.println("\n\n");
  Serial.println("╔════════════════════════════════════════════════════════════╗");
  Serial.println("║     ESP32 Local AI Security Node - Version 1.0             ║");
  Serial.println("║     Initializing system...                                 ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝");
  Serial.println();

  // Load saved WiFi settings
  loadWifiSettings();

  // Try to connect to saved WiFi
  if (!connectToPreferredNetwork()) {
    startAccessPoint();
  }

  // Initialize web server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/cmd", HTTP_POST, handleCmd);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("╔════════════════════════════════════════════════════════════╗");
  Serial.println("║                    READY FOR USE                           ║");
  Serial.println("╠════════════════════════════════════════════════════════════╣");
  Serial.printf("║ SSID:    %-54s║\n", currentWifiSsid.c_str());\n  Serial.printf(\"║ IP:      %-54s║\n\", currentIp.c_str());\n  Serial.printf(\"║ Mode:    %-54s║\n\", apMode ? \"AP Hotspot\" : \"WiFi Station\");\n  Serial.println(\"║                                                            ║\");\n  Serial.println(\"║ Open a web browser to:                                     ║\");\n  Serial.printf(\"║ http://%s                                    ║\n\", currentIp.c_str());\n  Serial.println(\"║                                                            ║\");\n  if (apMode) {\n    Serial.printf(\"║ WiFi SSID: %s                              ║\n\", AP_SSID);\n    Serial.printf(\"║ WiFi Password: %s                          ║\n\", AP_PASSWORD);\n  }\n  Serial.println(\"╚════════════════════════════════════════════════════════════╝\");\n  Serial.println();\n\n  // Set initial LED state\n  if (wifiConnected || apMode) {\n    setReadyIndicator();\n  } else {\n    setOfflineIndicator();\n  }\n}\n\n// ============================================================================\n// MAIN LOOP\n// ============================================================================\n\nvoid loop() {\n  // Handle incoming web requests\n  server.handleClient();\n\n  // Update LED indicator\n  if (busyFlag) {\n    // Flash LED while busy\n    setBusyIndicator();\n  } else if (wifiConnected || apMode) {\n    // Solid LED when ready\n    setReadyIndicator();\n  } else {\n    // LED off when offline\n    setOfflineIndicator();\n  }\n\n  // Health check: ensure we stay connected or have AP running\n  static unsigned long lastHealthCheck = 0;\n  if (millis() - lastHealthCheck > 15000) {\n    lastHealthCheck = millis();\n    \n    if (WiFi.status() != WL_CONNECTED) {\n      wifiConnected = false;\n      if (!apMode) {\n        Serial.println(\"[Health] WiFi disconnected. Starting AP mode.\");\n        startAccessPoint();\n      }\n    } else {\n      wifiConnected = true;\n      if (apMode) {\n        Serial.println(\"[Health] WiFi reconnected. Disabling AP mode.\");\n        apMode = false;\n      }\n    }\n  }\n\n  delay(LOOP_DELAY);\n}\n