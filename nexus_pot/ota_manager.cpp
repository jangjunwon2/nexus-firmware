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

const char* HTML_HEADER = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>Nexus Settings</title><style>:root{--primary-color:#3498db;--primary-hover-color:#2980b9;--danger-color:#e74c3c;--danger-hover-color:#c0392b;--light-gray-color:#ecf0f1;--medium-gray-color:#bdc3c7;--dark-gray-color:#2c3e50;--text-color:#34495e;--card-bg-color:#ffffff;--body-bg-color:#f4f4f9;--success-color:#27ae60}body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;margin:0;padding:20px;background-color:var(--body-bg-color);color:var(--text-color);display:flex;justify-content:center;align-items:flex-start;min-height:100vh}.container{width:100%;max-width:500px}h1{color:var(--dark-gray-color);text-align:center;margin-bottom:20px}h2{color:var(--dark-gray-color);margin-top:0;padding-bottom:10px;border-bottom:1px solid #eee}.card{background-color:var(--card-bg-color);padding:25px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.1);margin-bottom:20px}label{display:block;margin:15px 0 8px 0;font-weight:600;color:var(--dark-gray-color)}select,input[type="password"],input[type="text"]{width:100%;padding:12px;margin-bottom:15px;border:1px solid var(--medium-gray-color);border-radius:6px;font-size:16px;box-sizing:border-box;transition:border-color 0.2s}select:focus,input:focus{outline:none;border-color:var(--primary-color)}.btn,input[type="submit"]{display:inline-block;width:100%;background-color:var(--primary-color);color:white;padding:14px 20px;border:none;border-radius:6px;cursor:pointer;font-size:16px;font-weight:bold;text-align:center;text-decoration:none;transition:background-color 0.2s,box-shadow 0.2s;box-sizing:border-box}.btn:hover,input[type="submit"]:hover{background-color:var(--primary-hover-color);box-shadow:0 2px 6px rgba(0,0,0,0.15)}.button-group{display:flex;gap:10px;margin-top:15px}.btn-secondary{background-color:var(--light-gray-color);color:var(--dark-gray-color)}.btn-secondary:hover{background-color:#dbe0e2}.btn-danger{background-color:var(--danger-color)}.btn-danger:hover{background-color:var(--danger-hover-color)}.status-message,.error-message{padding:15px;border-radius:6px;margin:15px 0;text-align:center}.status-message{background-color:#e8f5e9;color:#2e7d32}.error-message{background-color:#ffebee;color:var(--danger-color);font-weight:bold}#version-card p,#network-status-card p{margin:8px 0}#connection-status.connected{color:var(--success-color);font-weight:bold}#connection-status.disconnected{color:var(--danger-color);font-weight:bold}</style></head><body><div class="container"><h1>Nexus Settings</h1>
)rawliteral";
const char* HTML_FOOTER = R"rawliteral(
</div></body></html>
)rawliteral";

static void wifiScanTask(void* pvParameters);

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
    WiFi.softAPConfig(OTA_AP_IP, OTA_AP_IP, OTA_AP_SUBNET);
    // [MODIFIED] No password for AP
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
            WiFi.disconnect(true);
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
    http.begin(client, OTA_FIRMWARE_URL);
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
            while (http.connected() && (written < contentLength)) {
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
                logPrintf(LogLevel::LOG_INFO, "OTA: Update complete, rebooting...");
                delay(1000);
                ESP.restart();
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

void setupWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = HTML_HEADER;
        html += R"rawliteral(
        <div class="card" id="network-status-card">
            <p><strong>Wi-Fi Status:</strong> <span id="connection-status">Checking...</span></p>
        </div>
        <div class="card" id="version-card" style="display:none;">
            <h2>Version Info</h2>
            <p>Current: <span id="current-version">N/A</span></p>
            <p>Latest: <span id="latest-version">N/A</span></p>
        </div>
        <div class='card' id='status-card'>
            <p class='status-message' id='status-message'>Loading...</p>
        </div>
        <div class='card'>
            <h2>Wi-Fi Setup</h2>
            <form id='connect-form' onsubmit='event.preventDefault(); handleConnect();'>
                <label for='ssid-select'>Wi-Fi Network:</label>
                <select name='ssid' id='ssid-select' required>
                    <option value=''>Scanning...</option>
                </select>
                <label for='password-input'>Password:</label>
                <input type='password' id='password-input' name='password' autocomplete='current-password' placeholder='Leave blank for open networks'>
                <div class='button-group'>
                    <button type='submit' id='action-btn' class='btn'>Connect</button>
                    <button type='button' id='rescan-btn' class='btn btn-secondary' onclick='scanWifi()'>Re-scan</button>
                </div>
            </form>
        </div>
        <script>
            let connectPollInterval;

            function setUIState(state, data = {}) {
                const actionBtn = document.getElementById('action-btn'), rescanBtn = document.getElementById('rescan-btn'), ssidSelect = document.getElementById('ssid-select'), passwordInput = document.getElementById('password-input'), statusMsg = document.getElementById('status-message'), connectionStatus = document.getElementById('connection-status'), versionCard = document.getElementById('version-card');
                actionBtn.disabled = false; rescanBtn.style.display = 'inline-block'; passwordInput.disabled = false; ssidSelect.disabled = false;
                switch(state) {
                    case 'disconnected': actionBtn.textContent = 'Connect'; actionBtn.className = 'btn'; statusMsg.textContent = 'Please select a network to connect.'; statusMsg.className = 'status-message'; versionCard.style.display = 'none'; connectionStatus.textContent = 'Not Connected'; connectionStatus.className = 'disconnected'; break;
                    case 'scanning': actionBtn.disabled = true; rescanBtn.disabled = true; rescanBtn.textContent = 'Scanning...'; ssidSelect.innerHTML = '<option value="">Searching...</option>'; connectionStatus.textContent = 'Scanning...'; connectionStatus.className = ''; break;
                    case 'scan_complete': rescanBtn.disabled = false; rescanBtn.textContent = 'Re-scan'; ssidSelect.innerHTML = '<option value="">-- Select a Network --</option>'; if (data.networks && data.networks.length > 0) { data.networks.slice(0, 20).forEach(net => { ssidSelect.add(new Option(`${net.ssid} (${net.rssi}dBm)`, net.ssid)); }); statusMsg.textContent = 'Scan complete. Select a network.'; } else { statusMsg.textContent = 'No networks found.'; } connectionStatus.textContent = 'Not Connected'; connectionStatus.className = 'disconnected'; break;
                    case 'connecting': actionBtn.disabled = true; rescanBtn.style.display = 'none'; actionBtn.textContent = 'Connecting...'; statusMsg.textContent = `Attempting to connect to ${data.ssid}...`; connectionStatus.textContent = `Connecting to ${data.ssid}...`; connectionStatus.className = ''; break;
                    case 'connected': actionBtn.textContent = 'Disconnect'; actionBtn.className = 'btn btn-danger'; rescanBtn.style.display = 'none'; passwordInput.disabled = true; ssidSelect.disabled = true; if(ssidSelect.value !== data.ssid) { ssidSelect.innerHTML = `<option value="${data.ssid}">${data.ssid}</option>`; } statusMsg.innerHTML = `Successfully connected. See device screen for details.`; statusMsg.className = 'status-message'; connectionStatus.innerHTML = `<strong>${data.ssid}</strong> (IP: ${data.ip})`; connectionStatus.className = 'connected'; versionCard.style.display = 'block'; break;
                    case 'failed': actionBtn.textContent = 'Connect'; actionBtn.className = 'btn'; statusMsg.textContent = 'Connection failed. Please check password and try again.'; statusMsg.className = 'error-message'; connectionStatus.textContent = 'Not Connected'; connectionStatus.className = 'disconnected'; rescanBtn.style.display = 'inline-block'; passwordInput.disabled = false; ssidSelect.disabled = false; break;
                }
            }

            function checkVersion() {
                document.getElementById('version-card').style.display = 'block';
                document.getElementById('latest-version').textContent = 'Checking...';
                fetch('/api/version').then(res => res.json()).then(data => {
                    if (data && data.current) {
                        document.getElementById('current-version').textContent = data.current;
                        document.getElementById('latest-version').textContent = data.latest || 'N/A';
                    }
                }).catch(err => {
                    document.getElementById('latest-version').textContent = 'Error';
                });
            }

            function handleConnect() { document.getElementById('action-btn').textContent === 'Connect' ? connectWifi() : disconnectWifi(); }
            
            function scanWifi() { 
                setUIState('scanning'); 
                fetch('/api/scan?rescan=1')
                    .then(response => {
                        if (!response.ok) throw new Error('Failed to trigger scan');
                        setTimeout(() => {
                            fetch('/api/scan')
                                .then(r => r.json())
                                .then(data => {
                                    if(data.error) throw new Error(data.error);
                                    setUIState('scan_complete', data);
                                })
                                .catch(e => {
                                    console.error('Scan results fetch failed:', e);
                                    setUIState('disconnected');
                                    document.getElementById('status-message').textContent = 'Failed to get scan results.';
                                });
                        }, 5000);
                    })
                    .catch(e => {
                        console.error('Scan trigger failed:', e);
                        setUIState('disconnected');
                        document.getElementById('status-message').textContent = 'Failed to start scan.';
                    });
            }
            function disconnectWifi() { fetch('/disconnect', { method: 'POST' }).then(() => { setUIState('disconnected'); setTimeout(scanWifi, 500); }); }
            
            function connectWifi() { 
                const ssid = document.getElementById('ssid-select').value, pass = document.getElementById('password-input').value; 
                if (!ssid) return; 
                setUIState('connecting', { ssid }); 
                const formData = new URLSearchParams({ssid, pass}); 
                fetch('/connect', { method: 'POST', body: formData }).then(res => res.json()).then(data => { 
                    if (data.success) { 
                        let attempts = 0; 
                        if(connectPollInterval) clearInterval(connectPollInterval); 
                        connectPollInterval = setInterval(() => { 
                            attempts++; 
                            fetch('/api/status').then(res => res.json()).then(statusData => { 
                                if (statusData.status === 'connected') { 
                                    clearInterval(connectPollInterval); 
                                    setUIState('connected', statusData); 
                                    checkVersion();
                                } else if (statusData.status === 'failed') { 
                                    clearInterval(connectPollInterval); 
                                    setUIState('failed'); 
                                    fetch('/reset_status', { method: 'POST' }); 
                                } else if (attempts > 15) { // Timeout increased to 7.5s
                                    clearInterval(connectPollInterval); 
                                    setUIState('failed'); 
                                    fetch('/reset_status', { method: 'POST' }); 
                                } 
                            }).catch(err => { clearInterval(connectPollInterval); setUIState('failed'); }); 
                        }, 500); 
                    } else { 
                        setUIState('failed'); 
                    } 
                }); 
            }

            window.onload = () => {
                if(connectPollInterval) clearInterval(connectPollInterval);
                fetch('/api/status').then(res => res.json()).then(data => {
                    if (data.connected) {
                        setUIState('connected', data);
                        checkVersion();
                    } else {
                        fetch('/api/scan')
                            .then(r => r.json())
                            .then(data => setUIState('scan_complete', data))
                            .catch(e => setUIState('disconnected'));
                    }
                }).catch(err => { scanWifi(); });
            };
        </script>
        )rawliteral";
        html += HTML_FOOTER;
        request->send(200, "text/html", html);
    });
    
    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("rescan")) {
            if (!scan_in_progress) {
                logPrintf(LogLevel::LOG_INFO, "WebAPI: Re-scan trigger received.");
                scan_in_progress = true;
                xTaskCreate(wifiScanTask, "wifiScanTask", 4096, nullptr, 5, NULL);
                request->send(200, "application/json", "{\"status\":\"Scan initiated\"}");
            } else {
                logPrintf(LogLevel::LOG_WARN, "WebAPI: Re-scan requested but a scan is already in progress.");
                request->send(503, "application/json", "{\"error\":\"Scan already in progress\"}");
            }
            return;
        }

        logPrintf(LogLevel::LOG_INFO, "WebAPI: Returning cached scan results.");
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
            logPrintf(LogLevel::LOG_INFO, "WebAPI: /connect requested for SSID: %s", ssid.c_str());
            wifi_password = pass;

            WiFi.disconnect(true);
            delay(200);

            otaWifiStatus = OTA_WIFI_CONNECTING;
            WiFi.begin(ssid.c_str(), pass.c_str());
            
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            logPrintf(LogLevel::LOG_WARN, "WebAPI: /connect request failed, SSID is missing.");
            request->send(400, "application/json", "{\"success\":false, \"message\":\"SSID required\"}");
        }
    });

    server.on("/disconnect", HTTP_POST, [](AsyncWebServerRequest *request){
        logPrintf(LogLevel::LOG_INFO, "WebAPI: /disconnect requested.");
        WiFi.disconnect(true);
        otaWifiStatus = OTA_WIFI_IDLE;
        if (isOtaMode(currentMode)) {
            currentMode = MODE_OTA_WIFI_AP;
            updateDisplay(); 
        }
        request->send(200, "application/json", "{\"success\":true}");
    });
    
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
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

    server.on("/reset_status", HTTP_POST, [](AsyncWebServerRequest *request){
        otaWifiStatus = OTA_WIFI_IDLE;
        request->send(200, "application/json", "{\"success\":true}");
    });
    
    server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request){
        logPrintf(LogLevel::LOG_INFO, "WebAPI: /api/version requested. Starting synchronous version check.");
        checkFirmwareVersion(); 
        JsonDocument doc;
        doc["current"] = firmwareVersion;
        doc["latest"] = otaState.latestVersion;
        String responseStr;
        serializeJson(doc, responseStr);
        request->send(200, "application/json", responseStr);
    });
}