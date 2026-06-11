// transmitter_s3/ota_manager.cpp

#include "config_t.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include "esp_task_wdt.h" // For watchdog timer reset

#include "ota_manager.h"
#include "utils_t.h"
#include <ESPAsyncWebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "hardware_display.h"
#include "espnow_t.h"
#include <algorithm>
#include <Update.h>
#include <vector>

// [Fix] ScanResult struct definition needed for vector
struct ScanResult {
    String ssid;
    int32_t rssi;
};

// [Fix] Forward declarations (함수 원형 선언 추가)
void checkFirmwareVersion();
void performOtaUpdate();

AsyncWebServer server(80);
static bool webServerInitialized = false;
static bool wifiEventHandlersRegistered = false;
static wifi_event_id_t wifi_event_handle;

static std::vector<ScanResult> cachedScanResults;
static bool scan_in_progress = false;

enum OtaWifiState { OTA_WIFI_IDLE, OTA_WIFI_CONNECTING, OTA_WIFI_CONNECTED, OTA_WIFI_FAILED };
static OtaWifiState otaWifiStatus = OTA_WIFI_IDLE;
static bool otaUpdateDownloaded = false;
static unsigned long wifiConnectStartMillis = 0;

String getPageHeader(const String& title) {
    String html = F("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    html += "<title>" + title + "</title>";
    html += F(R"rawliteral(<style>
        @import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap');
        
        body {
            font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
            margin: 0;
            padding: 20px 10px;
            background-color: #0b0b0f;
            background-image: radial-gradient(circle at 50% 0%, #1e1b4b 0%, #0b0b0f 70%);
            color: #f3f4f6;
            text-align: center;
            min-height: 100vh;
            box-sizing: border-box;
        }
        
        .container {
            max-width: 500px;
            margin: 40px auto;
            background: rgba(255, 255, 255, 0.02);
            padding: 30px 24px;
            border-radius: 24px;
            border: 1px solid rgba(255, 255, 255, 0.08);
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            box-shadow: 0 20px 50px rgba(0, 0, 0, 0.4);
            text-align: center;
        }
        
        h1 {
            font-size: 26px;
            font-weight: 700;
            margin-top: 0;
            margin-bottom: 25px;
            background: linear-gradient(135deg, #a78bfa 0%, #8b5cf6 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            letter-spacing: -0.5px;
        }
        
        h2, h3 {
            color: #e5e7eb;
            font-weight: 600;
            margin-top: 0;
            margin-bottom: 12px;
            font-size: 18px;
            letter-spacing: -0.2px;
        }
        
        .card {
            background: rgba(255, 255, 255, 0.015);
            padding: 20px;
            margin-bottom: 20px;
            border-radius: 16px;
            border: 1px solid rgba(255, 255, 255, 0.05);
            box-shadow: inset 0 1px 1px rgba(255, 255, 255, 0.03);
            text-align: center;
        }
        
        .btn {
            display: inline-flex;
            align-items: center;
            justify-content: center;
            background: linear-gradient(135deg, #6d28d9 0%, #5b21b6 100%);
            color: #ffffff;
            padding: 12px 24px;
            margin: 8px 4px;
            text-decoration: none;
            border: none;
            border-radius: 12px;
            cursor: pointer;
            font-size: 15px;
            font-weight: 600;
            min-width: 160px;
            box-shadow: 0 4px 12px rgba(109, 40, 217, 0.2);
            transition: all 0.25s cubic-bezier(0.4, 0, 0.2, 1);
        }
        
        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(109, 40, 217, 0.35);
            background: linear-gradient(135deg, #7c3aed 0%, #6d28d9 100%);
        }
        
        .btn:active {
            transform: translateY(0);
        }
        
        .btn:disabled {
            background: #4b5563;
            color: #9ca3af;
            box-shadow: none;
            cursor: not-allowed;
            transform: none;
        }
        
        .btn-danger {
            background: linear-gradient(135deg, #dc2626 0%, #b91c1c 100%);
            box-shadow: 0 4px 12px rgba(220, 38, 38, 0.2);
        }
        
        .btn-danger:hover {
            background: linear-gradient(135deg, #ef4444 0%, #dc2626 100%);
            box-shadow: 0 6px 20px rgba(220, 38, 38, 0.35);
        }
        
        .btn-secondary {
            background: rgba(255, 255, 255, 0.05);
            color: #e5e7eb;
            border: 1px solid rgba(255, 255, 255, 0.1);
            box-shadow: none;
        }
        
        .btn-secondary:hover {
            background: rgba(255, 255, 255, 0.1);
            box-shadow: none;
        }
        
        .btn-success {
            background: linear-gradient(135deg, #059669 0%, #047857 100%);
            box-shadow: 0 4px 12px rgba(5, 150, 105, 0.2);
        }
        
        .btn-success:hover {
            background: linear-gradient(135deg, #10b981 0%, #059669 100%);
            box-shadow: 0 6px 20px rgba(5, 150, 105, 0.35);
        }
        
        input, select {
            width: calc(100% - 28px);
            padding: 12px 14px;
            margin: 10px 0;
            background: rgba(0, 0, 0, 0.3);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 12px;
            color: #ffffff;
            font-size: 15px;
            text-align: center;
            outline: none;
            transition: all 0.2s ease;
        }
        
        input:focus, select:focus {
            border-color: #a78bfa;
            box-shadow: 0 0 0 3px rgba(167, 139, 250, 0.2);
            background: rgba(0, 0, 0, 0.4);
        }
        
        .hidden {
            display: none;
        }
        
        .form-group {
            margin-bottom: 20px;
            text-align: left;
        }
        
        .form-group label {
            display: block;
            margin-bottom: 6px;
            font-size: 14px;
            color: #9ca3af;
            font-weight: 500;
        }
        
        .message-box {
            padding: 14px 20px;
            border-radius: 12px;
            margin-top: 15px;
            text-align: center;
            transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
            position: fixed;
            top: 20px;
            left: 50%;
            transform: translate(-50%, -20px);
            z-index: 1000;
            width: 85%;
            max-width: 420px;
            display: none;
            opacity: 0;
            box-shadow: 0 10px 30px rgba(0, 0, 0, 0.5);
            font-size: 14px;
            font-weight: 500;
        }
        
        .message-box.show {
            display: block;
            opacity: 1;
            transform: translate(-50%, 0);
        }
        
        .message-info {
            background-color: #1e3a8a;
            border: 1px solid #2563eb;
            color: #bfdbfe;
        }
        
        .message-success {
            background-color: #064e3b;
            border: 1px solid #059669;
            color: #a7f3d0;
        }
        
        .message-error {
            background-color: #7f1d1d;
            border: 1px solid #dc2626;
            color: #fca5a5;
        }
        
        .changelog {
            text-align: left;
            background: rgba(0, 0, 0, 0.4);
            padding: 15px;
            border-radius: 12px;
            margin-bottom: 15px;
            border: 1px solid rgba(255, 255, 255, 0.05);
            white-space: pre-wrap;
            font-size: 13px;
            line-height: 1.5;
            color: #d1d5db;
        }
        
        .progress-bar {
            width: 90%;
            max-width: 350px;
            background-color: rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            overflow: hidden;
            margin: 15px auto;
            border: 1px solid rgba(255, 255, 255, 0.05);
        }
        
        .progress-bar-inner {
            height: 14px;
            width: 0%;
            background: linear-gradient(90deg, #10b981, #059669);
            color: white;
            text-align: center;
            line-height: 14px;
            font-size: 10px;
            font-weight: 700;
            transition: width 0.2s ease;
        }
        
        .notice {
            font-size: 13px;
            color: #f87171;
            margin-top: 10px;
            font-weight: 500;
        }
    </style>
    <script>
        function showMessage(text, type = 'info', duration = 3000) {
            let msgBox = document.getElementById('global-message-box');
            if (!msgBox) {
                msgBox = document.createElement('div');
                msgBox.id = 'global-message-box';
                document.body.appendChild(msgBox);
            }
            msgBox.textContent = text;
            msgBox.className = 'message-box message-' + type + ' show';
            if (duration > 0) {
                setTimeout(() => {
                    msgBox.classList.remove('show');
                }, duration);
            }
        }
    </script>
    </head><body><div class='container'><h1>)rawliteral");
    html += title; html += F("</h1>"); return html;
}

String getPageFooter(bool showHomeButton) {
    String html; 
    if (showHomeButton) html += F("<p style='margin-top:25px;'><a href='/' class='btn'>Back to Home</a></p>");
    html += F("</div></body></html>"); 
    return html;
}

static void wifiScanTask(void* pvParameters) {
    logPrintf(LogLevel::LOG_INFO, "OTA TASK: Performing Wi-Fi scan asynchronously.");
    int n = WiFi.scanNetworks(false, false);
    cachedScanResults.clear();
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            cachedScanResults.push_back({WiFi.SSID(i), WiFi.RSSI(i)});
        }
        std::sort(cachedScanResults.begin(), cachedScanResults.end(), [](const auto &a, const auto &b) { return a.rssi > b.rssi; });
        logPrintf(LogLevel::LOG_INFO, "OTA TASK: Scan found %d networks.", n);
    } else {
        logPrintf(LogLevel::LOG_WARN, "OTA TASK: Scan found no networks.");
    }
    WiFi.scanDelete();
    scan_in_progress = false;
    vTaskDelete(NULL);
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            logPrintf(LogLevel::LOG_INFO, "WiFi Connected! IP: %s", WiFi.localIP().toString().c_str());
            otaWifiStatus = OTA_WIFI_CONNECTED;
            saveKnownNetwork(WiFi.SSID(), wifi_password);
            currentMode = MODE_OTA_CHECKING;
            updateDisplay();
            delay(500);
            otaState.inProgress = true;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (otaWifiStatus == OTA_WIFI_CONNECTING) {
                logPrintf(LogLevel::LOG_WARN, "WiFi Connection Failed. Reason: %d", info.wifi_sta_disconnected.reason);
                otaWifiStatus = OTA_WIFI_FAILED;
                WiFi.disconnect(false);
            } else if (otaWifiStatus == OTA_WIFI_CONNECTED) {
                logPrintf(LogLevel::LOG_WARN, "WiFi Lost Connection. Reason: %d", info.wifi_sta_disconnected.reason);
                otaWifiStatus = OTA_WIFI_IDLE;
                if (isOtaMode(currentMode)) {
                    currentMode = MODE_OTA_WIFI_AP;
                    updateDisplay();
                }
            }
            break;
        default: break;
    }
}

static void startOtaWebApp() {
    logPrintf(LogLevel::LOG_INFO, "OTA: Starting AP+STA mode and web server.");
    currentMode = MODE_OTA_WIFI_AP;
    otaWifiStatus = OTA_WIFI_IDLE;
    
    if (!wifiEventHandlersRegistered) {
        wifi_event_handle = WiFi.onEvent(onWiFiEvent);
        wifiEventHandlersRegistered = true;
    }
    
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    WiFi.softAPConfig(OTA_AP_IP, OTA_AP_IP, OTA_AP_SUBNET);
    WiFi.softAP(OTA_AP_PREFIX, NULL, OTA_AP_CHANNEL, 0, OTA_AP_MAX_CONN);
    logPrintf(LogLevel::LOG_INFO, "OTA: SoftAP '%s' started at %s (No Password)", OTA_AP_PREFIX, WiFi.softAPIP().toString().c_str());
    
    if (!webServerInitialized) {
        setupWebServer();
        webServerInitialized = true;
    }
    server.begin();
    updateDisplay();
}

static bool attemptAutoConnect() {
    loadKnownNetworks(); 
    if (knownNetworks.empty()) {
        logPrintf(LogLevel::LOG_INFO, "OTA: No known networks for auto-connect.");
        return false;
    }

    logPrintf(LogLevel::LOG_INFO, "OTA: Found %d known networks. Attempting auto-connect...", knownNetworks.size());

    for (const auto& net : knownNetworks) {
        logPrintf(LogLevel::LOG_INFO, "OTA: Trying to connect to '%s'...", net.ssid.c_str());
        
        otaConnectingSsid = net.ssid;
        currentMode = MODE_OTA_CONNECTING;
        updateDisplay();
        
        wifi_password = net.pass;
        WiFi.begin(net.ssid.c_str(), net.pass.c_str());

        unsigned long startAttemptTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_CONNECT_TIMEOUT_MS) {
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
            logPrintf(LogLevel::LOG_INFO, "OTA: Auto-connect to '%s' successful!", net.ssid.c_str());
            otaConnectingSsid = "";
            return true;
        } else {
            logPrintf(LogLevel::LOG_WARN, "OTA: Failed to connect to '%s'. Trying next...", net.ssid.c_str());
            WiFi.disconnect(false);
            delay(100);
        }
    }

    logPrintf(LogLevel::LOG_WARN, "OTA: Auto-connect failed for all known networks.");
    otaConnectingSsid = "";
    return false;
}

void initOtaWorkflow() {
    logPrintf(LogLevel::LOG_INFO, "OTA: Entering workflow.");
    otaWorkflowActive = true;
    modeHistory.push_back(currentMode);
    
    deinitEspNow();
    
    currentMode = MODE_OTA_SCANNING;
    updateDisplay();
    delay(100);

    logPrintf(LogLevel::LOG_INFO, "OTA: Performing initial Wi-Fi scan in STA mode...");
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, false);
    
    cachedScanResults.clear();
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            cachedScanResults.push_back({WiFi.SSID(i), WiFi.RSSI(i)});
        }
        std::sort(cachedScanResults.begin(), cachedScanResults.end(), [](const auto &a, const auto &b) { return a.rssi > b.rssi; });
    }
    WiFi.scanDelete();
    logPrintf(LogLevel::LOG_INFO, "OTA: Initial scan found %d networks.", n);

    WiFi.mode(WIFI_OFF);
    delay(100);
    startOtaWebApp();

    if (!attemptAutoConnect()) {
        logPrintf(LogLevel::LOG_INFO, "OTA: Auto-connect failed. AP is active for manual setup.");
        currentMode = MODE_OTA_WIFI_AP;
        otaConnectingSsid = "";
        updateDisplay();
    }
}

void handleOtaLoop() {
    if (currentMode == MODE_OTA_CHECKING && otaState.inProgress) {
        otaState.inProgress = false;
        checkFirmwareVersion();
    }

    if (currentMode == MODE_OTA_DOWNLOADING && otaState.updateConfirmed) {
        otaState.updateConfirmed = false;
        performOtaUpdate();
    }

    if (otaWifiStatus == OTA_WIFI_CONNECTING && (millis() - wifiConnectStartMillis > WIFI_CONNECT_TIMEOUT_MS)) {
        logPrintf(LogLevel::LOG_WARN, "OTA: Wi-Fi connection timed out.");
        otaWifiStatus = OTA_WIFI_FAILED;
        WiFi.disconnect(false);
    }
}

void shutdownWifi() {
    server.end();
    webServerInitialized = false;
    
    if(wifiEventHandlersRegistered) {
        WiFi.removeEvent(wifi_event_handle);
        wifiEventHandlersRegistered = false;
    }

    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    
    otaWifiStatus = OTA_WIFI_IDLE;
    otaWorkflowActive = false;
    logPrintf(LogLevel::LOG_INFO, "OTA: Wi-Fi interfaces have been shut down.");
}

void checkFirmwareVersion() {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    
    http.begin(client, OTA_VERSION_URL);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc; 
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error) {
            otaState.latestVersion = doc["version"].as<String>();
            otaState.changeLog = doc["changelog"].as<String>();
            otaState.firmwareUrl = doc["url"].as<String>();
            otaState.updateAvailable = (otaState.latestVersion != firmwareVersion);
            currentMode = MODE_UPDATE_PAGE;
            updateDisplay();
        } else {
            logPrintf(LogLevel::LOG_ERROR, "OTA: Version JSON parsing failed: %s", error.c_str());
            otaErrorMessage = "Version parse failed";
            currentMode = MODE_OTA_ERROR;
            updateDisplay();
        }
    } else {
        logPrintf(LogLevel::LOG_ERROR, "OTA: Version HTTP request failed: %d", httpCode);
        otaErrorMessage = "Version check failed";
        currentMode = MODE_OTA_ERROR;
        updateDisplay();
    }
    http.end();
}

void performOtaUpdate() {
    if (otaState.inProgress) return;
    otaState.inProgress = true;
    otaState.downloadProgress = 0;
    logPrintf(LogLevel::LOG_INFO, "OTA: Starting firmware update...");
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (otaState.firmwareUrl.isEmpty()) {
        otaErrorMessage = "No firmware URL";
        currentMode = MODE_OTA_ERROR;
        otaState.inProgress = false;
        updateDisplay();
        return;
    }
    http.begin(client, otaState.firmwareUrl);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        if (contentLength > 0) {
            if (!Update.begin(contentLength)) {
                otaErrorMessage = "Not enough space";
                currentMode = MODE_OTA_ERROR;
                otaState.inProgress = false;
                http.end();
                return;
            }
            WiFiClient *stream = http.getStreamPtr();
            size_t written = 0;
            uint8_t buff[128] = {0};
            while (http.connected() && (written < (size_t)contentLength)) {
                size_t size = stream->available();
                if (size) {
                    size_t read = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
                    if (Update.write(buff, read) != read) {
                        otaErrorMessage = "Write failed";
                        currentMode = MODE_OTA_ERROR;
                        otaState.inProgress = false;
                        http.end();
                        return;
                    }
                    written += read;
                    otaState.downloadProgress = (written * 100) / contentLength;
                    updateDisplay();
                }
                delay(1);
            }
            if (Update.end()) {
                logPrintf(LogLevel::LOG_INFO, "OTA: Update complete, waiting for exit to reboot...");
                otaUpdateDownloaded = true;
                currentMode = MODE_OTA_SUCCESS;
            } else {
                otaErrorMessage = "Update failed";
                currentMode = MODE_OTA_ERROR;
            }
        } else {
            otaErrorMessage = "Invalid file size";
            currentMode = MODE_OTA_ERROR;
        }
    } else {
        otaErrorMessage = "Download failed";
        currentMode = MODE_OTA_ERROR;
    }
    http.end();
    otaState.inProgress = false;
    updateDisplay();
}

void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = getPageHeader("Nexus Transmitter");
        html += "<div class='card'><h3>Wi-Fi Status</h3><p id='home-wifi-status'>Loading...</p></div>";
        html += "<div class='card'><h3>Device Control</h3>"
                "<p><a href='/wifi' class='btn'>Wi-Fi Settings</a></p>"
                "<p><a href='/update' class='btn'>Firmware Update</a></p>"
                "<p><a href='/exit' class='btn btn-danger'>Exit Wi-Fi Mode</a></p>"
                "</div>";
        html += getPageFooter(false);
        html += R"rawliteral(
        <script>
            function fetchWifiStatus() {
                fetch("/api/wifi-status").then(res => res.json()).then(d => {
                    let s = document.getElementById("home-wifi-status");
                    if (d.connected) {
                        s.innerHTML = "Connected to <b>" + d.ssid + "</b><br>IP Address: " + d.ip;
                    } else {
                        s.textContent = "Not connected. AP Mode is active.";
                    }
                }).catch(err => {
                    document.getElementById("home-wifi-status").textContent = "Error fetching status";
                });
            }
            window.onload = () => {
                fetchWifiStatus();
                setInterval(fetchWifiStatus, 3000);
            };
        </script>
        )rawliteral";
        request->send(200, "text/html; charset=UTF-8", html);
    });

    server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = getPageHeader("WiFi Settings");
        html += R"rawliteral(
        <div class="card">
            <h2>Current WiFi Status</h2>
            <p id="conn-status">Loading...</p>
        </div>
        <div class="card">
            <h2>WiFi Connection</h2>
            <div class="form-group" style="text-align: center;">
                <label for="ssid-select">Select SSID:</label>
                <select id="ssid-select">
                    <option value="">-- Scan to select a network --</option>
                </select>
            </div>
            <div style="display: flex; justify-content: center; margin-top: 20px; gap: 10px;">
                <button id="scan-btn" class="btn" onclick="scanWifi()">Rescan</button>
            </div>
            <div class="form-group" style="text-align: center;">
                <label for="password-input">Password:</label>
                <input type="password" id="password-input">
            </div>
            <button id="action-btn" class="btn" onclick="handleConnectDisconnect()">Connect</button> 
        </div>
        <script>
            const scanBtn = document.getElementById("scan-btn");
            const actionBtn = document.getElementById("action-btn"); 
            const ssidSelect = document.getElementById("ssid-select");
            const passwordInput = document.getElementById("password-input");
            const connStatusEl = document.getElementById("conn-status");
            let currentSsid = '';
            
            function fetchStatus() { 
                fetch("/api/wifi-status")
                    .then(response => response.json())
                    .then(data => handleWifiStatus(data))
                    .catch(err => console.error('Error fetching status:', err));
            }

            function handleConnectDisconnect() {
                if (actionBtn.textContent === "Connect") {
                    connectWifi();
                } else if (actionBtn.textContent === "Disconnect") {
                    disconnectWifi();
                }
            }

            function handleWifiStatus(data) {
                currentSsid = data.connected ? data.ssid : '';
                actionBtn.disabled = false;
                scanBtn.disabled = false;
                passwordInput.disabled = false;
                ssidSelect.disabled = false;
                actionBtn.classList.remove('btn-danger', 'btn-success');

                if (data.connected) {
                    connStatusEl.innerHTML = `Connected to <b>${data.ssid}</b> (IP: ${data.ip})`;
                    actionBtn.textContent = "Disconnect";
                    actionBtn.classList.add('btn-danger');
                    passwordInput.disabled = true;
                    ssidSelect.disabled = true;
                    scanBtn.disabled = true;
                } else {
                    connStatusEl.textContent = "Not Connected";
                    actionBtn.textContent = "Connect";
                    actionBtn.classList.add('btn');
                    
                    if (data.status === "connecting") {
                        showMessage("Connecting to selected WiFi...", 'info', 0);
                        actionBtn.disabled = true; 
                        scanBtn.disabled = true;
                        passwordInput.disabled = true;
                        ssidSelect.disabled = true;
                    } else if (data.status === "failed") {
                        showMessage("Connection failed. Check password.", 'error');
                    } else if (data.status === "disconnected") {
                        showMessage("Disconnected from WiFi.", 'info'); 
                    }
                }
            }

            let scanPollInterval;
            function scanWifi() {
                if (scanBtn.disabled) return;
                scanBtn.disabled = true;
                ssidSelect.innerHTML = "<option>Scanning...</option>";
                showMessage("Scanning for WiFi networks...", "info");
                fetch("/api/scan-wifi")
                    .then(() => {
                        if (scanPollInterval) clearInterval(scanPollInterval);
                        let pollCount = 0;
                        scanPollInterval = setInterval(() => {
                            pollCount++;
                            fetch("/api/scan")
                                .then(r => r.json())
                                .then(data => {
                                    if ((data.networks && data.networks.length > 0) || pollCount > 5) {
                                        clearInterval(scanPollInterval);
                                        const selectedSSID = ssidSelect.value;
                                        ssidSelect.innerHTML = "<option value=''>-- Select a Network --</option>";
                                        data.networks.slice(0, 20).forEach(net => {
                                            const option = new Option(net.ssid + " (" + net.rssi + " dBm)", net.ssid);
                                            ssidSelect.add(option);
                                        });
                                        if (selectedSSID) ssidSelect.value = selectedSSID;
                                        scanBtn.disabled = false;
                                        showMessage("Scan complete.", "success");
                                    }
                                }).catch(e => {
                                    clearInterval(scanPollInterval);
                                    scanBtn.disabled = false;
                                });
                        }, 2000);
                    })
                    .catch(error => {
                        showMessage("Failed to start scan.", "error");
                        scanBtn.disabled = false;
                    });
            }
            
            function connectWifi() {
                const ssid = ssidSelect.value;
                if (!ssid) {
                    showMessage("Please select a network first.", "error");
                    return;
                }
                actionBtn.disabled = true;
                scanBtn.disabled = true;
                passwordInput.disabled = true;
                ssidSelect.disabled = true;
                showMessage("Attempting to connect...", "info", 0);
                const password = passwordInput.value;
                
                let formData = new URLSearchParams();
                formData.append('ssid', ssid);
                formData.append('pass', password);

                fetch("/connect", {
                    method: "POST",
                    body: formData
                }).catch(err => {
                    showMessage("Failed to connect.", "error");
                });
            }

            function disconnectWifi() {
                if (!window.confirm("Are you sure you want to disconnect?")) return;
                actionBtn.disabled = true;
                showMessage('Disconnecting...', 'info', 0);
                fetch("/disconnect", { method: "POST" }).then(() => {
                    passwordInput.value = '';
                });
            }

            window.onload = () => {
                fetchStatus();
                setInterval(fetchStatus, 2000);
                setTimeout(scanWifi, 500);
            };
        </script>
        )rawliteral";
        html += getPageFooter(true);
        request->send(200, "text/html; charset=UTF-8", html);
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = getPageHeader("Firmware Update");
        html += R"rawliteral(
        <div class='card'>
            <p>Current Version: <b id='current-v'>-</b><br>Latest on Server: <b id='latest-v'>-</b></p>
            <div id='update-info'>
                 <div id='changelog' class='changelog'></div>
                 <p id='update-status'></p>
            </div>
            <div class="form-group" style="text-align: center;">
                <button id='update-btn' class='btn hidden' onclick='downloadUpdate()'>Update</button>
                <div id='download-progress' class='hidden' style='margin-top: 10px; display: flex; flex-direction: column; align-items: center;'>
                    <span id='progress-text' style='font-weight: bold;'>0%</span>
                    <div class='progress-bar'>
                        <div class='progress-bar-inner' id='progress-bar-inner'></div>
                    </div>
                </div>
                <p id='download-notice' class='hidden notice'>Firmware is downloading. The device will reboot when done.</p>
            </div>
        </div>
        <script>
            const updateBtn = document.getElementById('update-btn');
            const updateStatus = document.getElementById('update-status');
            const changelogEl = document.getElementById('changelog');
            const downloadNotice = document.getElementById('download-notice');
            const downloadProgressDiv = document.getElementById('download-progress');
            const progressBarInner = document.getElementById('progress-bar-inner');
            const progressText = document.getElementById('progress-text');

            function checkOtaStatus() {
                fetch("/api/ota-status").then(res => res.json()).then(d => {
                    document.getElementById("current-v").textContent = d.current_version;
                    document.getElementById("latest-v").textContent = d.latest_version;
                    changelogEl.textContent = d.changelog || "No changelog available.";

                    if (d.downloaded) {
                        updateStatus.innerHTML = "<b style='color:green;'>Update Complete! It will be applied when you exit Wi-Fi mode.</b>";
                        updateBtn.classList.add("hidden");
                        downloadProgressDiv.classList.add('hidden');
                        downloadNotice.classList.remove("hidden");
                        downloadNotice.textContent = "Firmware downloaded. Please go back and select 'Exit Wi-Fi Mode' to apply the update.";
                    } else if (d.in_progress) {
                        updateBtn.classList.add("hidden");
                        downloadNotice.classList.remove("hidden");
                        downloadProgressDiv.classList.remove('hidden');
                        progressText.textContent = d.progress + "%";
                        progressBarInner.style.width = d.progress + "%";
                    } else if (d.update_available) {
                        updateStatus.innerHTML = "<b style='color:green;'>Update available!</b>";
                        updateBtn.classList.remove("hidden");
                        updateBtn.disabled = false;
                        downloadProgressDiv.classList.add('hidden');
                        downloadNotice.classList.add("hidden");
                    } else {
                        updateStatus.textContent = "You are on the latest version.";
                        updateBtn.classList.add("hidden");
                        downloadProgressDiv.classList.add('hidden');
                        downloadNotice.classList.add("hidden");
                    }
                }).catch(err => console.error("OTA status check failed", err));
            }

            function downloadUpdate() {
                if (window.confirm("Start download? The update will be applied when you exit Wi-Fi mode.")) {
                    updateBtn.disabled = true;
                    downloadNotice.classList.remove("hidden");
                    downloadProgressDiv.classList.remove('hidden');
                    fetch("/api/download-ota", { method: "POST" });
                }
            }

            window.onload = () => {
                fetch("/api/check-ota");
                checkOtaStatus();
                setInterval(checkOtaStatus, 1000);
            };
        </script>
        )rawliteral";
        html += getPageFooter(true);
        request->send(200, "text/html; charset=UTF-8", html);
    });

    server.on("/exit", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = getPageHeader("Exiting Wi-Fi Mode");
        html += "<p>The device will now return to normal operation. You can close this window.</p>";
        if (otaUpdateDownloaded) {
            html += "<p style='color:blue;font-weight:bold;'>An update was downloaded and will be applied on reboot.</p>";
        }
        html += getPageFooter(false);
        request->send(200, "text/html; charset=UTF-8", html);
        delay(100);
        shutdownWifi();
        if (otaUpdateDownloaded) {
            logPrintf(LogLevel::LOG_INFO, "OTA: Rebooting to apply update...");
            Serial.end();
            delay(200);
            ESP.restart();
        } else {
            if (!initEspNow()) currentMode = MODE_ERROR;
            else currentMode = MODE_HOME_MENU;
            modeHistory.clear();
            updateDisplay();
        }
    });

    server.on("/api/wifi-status", HTTP_GET, [](AsyncWebServerRequest *request){
        String statusStr = "idle";
        if (otaWifiStatus == OTA_WIFI_CONNECTING) statusStr = "connecting";
        else if (otaWifiStatus == OTA_WIFI_CONNECTED) statusStr = "connected";
        else if (otaWifiStatus == OTA_WIFI_FAILED) statusStr = "failed";
        
        JsonDocument doc;
        doc["status"] = statusStr;
        doc["connected"] = (otaWifiStatus == OTA_WIFI_CONNECTED);
        if (otaWifiStatus == OTA_WIFI_CONNECTED) {
            doc["ssid"] = WiFi.SSID();
            doc["ip"] = WiFi.localIP().toString();
        }
        String responseStr;
        serializeJson(doc, responseStr);
        request->send(200, "application/json", responseStr);
    });

    server.on("/api/scan-wifi", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!scan_in_progress) {
            scan_in_progress = true;
            xTaskCreate(wifiScanTask, "wifiScanTask", 4096, nullptr, 5, NULL);
            request->send(200, "application/json", "{\"status\":\"Scan initiated\"}");
        } else {
            request->send(503, "application/json", "{\"error\":\"Scan already in progress\"}");
        }
    });

    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        JsonArray networks = doc["networks"].to<JsonArray>();
        for (const auto& net : cachedScanResults) {
            JsonObject obj = networks.add<JsonObject>();
            obj["ssid"] = net.ssid;
            obj["rssi"] = net.rssi;
        }
        String responseStr;
        serializeJson(doc, responseStr);
        request->send(200, "application/json", responseStr);
    });

    server.on("/connect", HTTP_POST, [](AsyncWebServerRequest *request){
        String ssid, pass;
        if (request->hasParam("ssid", true)) ssid = request->getParam("ssid", true)->value();
        if (request->hasParam("pass", true)) pass = request->getParam("pass", true)->value();
        if (ssid.length() > 0) {
            wifi_password = pass;
            WiFi.disconnect(false);
            delay(200);
            otaWifiStatus = OTA_WIFI_CONNECTING;
            wifiConnectStartMillis = millis();
            WiFi.begin(ssid.c_str(), pass.c_str());
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(400, "application/json", "{\"success\":false}");
        }
    });

    server.on("/disconnect", HTTP_POST, [](AsyncWebServerRequest *request){
        WiFi.disconnect(false);
        otaWifiStatus = OTA_WIFI_IDLE;
        if (isOtaMode(currentMode)) {
            currentMode = MODE_OTA_WIFI_AP;
            updateDisplay(); 
        }
        request->send(200, "application/json", "{\"success\":true}");
    });

    server.on("/api/check-ota", HTTP_GET, [](AsyncWebServerRequest *request){
        xTaskCreate([](void* p){ checkFirmwareVersion(); vTaskDelete(NULL); }, "checkOtaTask", 4096, NULL, 5, NULL);
        request->send(200, "application/json", "{\"status\":\"checking\"}");
    });

    server.on("/api/download-ota", HTTP_POST, [](AsyncWebServerRequest *request){
        otaState.updateConfirmed = true;
        currentMode = MODE_OTA_DOWNLOADING;
        request->send(200, "application/json", "{\"status\":\"download_started\"}");
    });

    server.on("/api/ota-status", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["current_version"] = firmwareVersion;
        doc["latest_version"] = otaState.latestVersion.isEmpty() ? "N/A" : otaState.latestVersion;
        doc["update_available"] = otaState.updateAvailable;
        doc["in_progress"] = otaState.inProgress;
        doc["progress"] = otaState.downloadProgress;
        doc["changelog"] = otaState.changeLog.isEmpty() ? "No change log." : otaState.changeLog;
        doc["error"] = otaErrorMessage;
        doc["downloaded"] = otaUpdateDownloaded;
        String responseStr;
        serializeJson(doc, responseStr);
        request->send(200, "application/json", responseStr);
    });
}