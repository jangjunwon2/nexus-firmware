/**
 * @file web.cpp
 * @brief WebManager Class Implementation. Manages AP mode, web server, API, and OTA.
 * @version 8.4.0
 * @date 2024-06-17
 */

#include "web.h"
#include "mode.h"
#include "comm.h"
#include "utils.h"
#include <vector>
#include <algorithm>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

// External declaration for global instance defined in .ino file
// WebManager webManager;

// WebManager의 static 인스턴스 포인터 초기화
WebManager* WebManager::_instance = nullptr;

WebManager::WebManager() :
    _server(80), _ws("/ws"), _modeManager(nullptr), _commManager(nullptr),
    _isServerRunning(false), _otaUpdateDownloaded(false), 
    _isScanningWifi(false), _isConnectingWifi(false),
    _currentFirmwareVersion(FIRMWARE_VERSION), _latestOtaVersion("N/A"), _otaChangeLog("N/A"), _otaUpdateAvailable(false),
    _wifiEventId(0), _lastDisconnectReason(0), _wifiConnectStartMillis(0),
    _disconnectedForTestSsid(""), _reconnectOnExitTest(false)
{
    _otaDataMutex = xSemaphoreCreateMutex();
}

void WebManager::begin(ModeManager* modeMgr, CommManager* commMgr) {
    _instance = this;
    _modeManager = modeMgr;
    _commManager = commMgr;
    
    setupRoutes();
    setupWebSocket();
    setupLogBroadcaster();
    
    xTaskCreatePinnedToCore(
        [](void* param) { static_cast<WebManager*>(param)->loop(); },
        "WebManagerLoop", 4096, this, 1, NULL, 1
    );
    Log::Info(PSTR("WEB: WebManager initialized."));
}

void WebManager::loop() {
    for (;;) {
        if (_isServerRunning.load()) {
            _ws.cleanupClients();
            if (_isConnectingWifi.load() && (millis() - _wifiConnectStartMillis > WIFI_CONNECT_TIMEOUT_MS)) {
                _isConnectingWifi = false;
                Log::Warn(PSTR("WEB: WiFi connection timed out."));
                WiFi.disconnect(true, true); 

                int reasonForBroadcast = (_lastDisconnectReason != 0) ? _lastDisconnectReason : 204;
                broadcastWifiStatus("failed", reasonForBroadcast);
                _lastDisconnectReason = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void WebManager::startServer() {
    if (_isServerRunning.load()) return;
    Log::Info(PSTR("WEB: Starting web server..."));
    uint8_t channel = _commManager->getChannel();
    WiFi.mode(WIFI_AP_STA);
    IPAddress apIP(AP_IP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASSWORD, channel, 0, 4);
    _wifiEventId = WiFi.onEvent(WebManager::onWiFiEvent);
    _server.begin();
    _isServerRunning = true;
    _otaUpdateDownloaded = false;
    if (_modeManager) _modeManager->setUpdateDownloaded(false);
    Log::Info(PSTR("WEB: Server started. AP SSID: %s, Channel: %d"), AP_SSID, channel);
    Log::Info(PSTR("WEB: Access Point IP: http://%s"), WiFi.softAPIP().toString().c_str());
}

void WebManager::stopServer() {
    if (!_isServerRunning.load()) return;
    Log::Info(PSTR("WEB: Stopping web server..."));

    // [수정] 서버 객체를 정리하기 전에 루프가 더 이상 실행되지 않도록 플래그를 먼저 설정합니다.
    _isServerRunning = false;

    // 잠시 대기하여 WebManagerLoop가 플래그 변경을 인지할 시간을 줍니다.
    vTaskDelay(pdMS_TO_TICKS(10)); 

    _ws.closeAll();
    _server.end();

    // 등록했던 WiFi 이벤트 핸들러를 안전하게 제거합니다.
    if (_wifiEventId != 0) {
        WiFi.removeEvent(_wifiEventId);
        _wifiEventId = 0;
    }

    Log::Info(PSTR("WEB: Server stopped."));
}

void WebManager::reconnectWifiIfNeeded() {
    if (_reconnectOnExitTest && !_disconnectedForTestSsid.isEmpty()) {
        Log::Info(PSTR("WEB: Attempting to reconnect to Wi-Fi (%s) after exiting test mode."), _disconnectedForTestSsid.c_str());
        String pass = Utils::loadWifiPassword(_disconnectedForTestSsid);

        if (!pass.isEmpty()) {
            WiFi.begin(_disconnectedForTestSsid.c_str(), pass.c_str());
        } else {
            Log::Warn(PSTR("WEB: Could not find Wi-Fi password for reconnection (%s)."), _disconnectedForTestSsid.c_str());
        }
        _disconnectedForTestSsid = "";
        _reconnectOnExitTest = false;
    }
}

bool WebManager::isServerRunning() const { return _isServerRunning.load(); }

void WebManager::performUpdateAndReboot() {
    if (_otaUpdateDownloaded.load()) {
        Log::Info(PSTR("WEB: Applying OTA update and rebooting..."));
        ESP.restart();
    }
}

void WebManager::broadcastTestComplete() {
    JsonDocument doc;
    doc["type"] = "test_completed";
    broadcastJson(doc);
}

// --- Page and API Handler Implementations ---

void WebManager::setupRoutes() {
    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r){ handleRoot(r); });
    _server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest* r){ handleWifiConfigPage(r); });
    _server.on("/update", HTTP_GET, [this](AsyncWebServerRequest* r){ handleFirmwareUpdatePage(r); });
    _server.on("/test", HTTP_GET, [this](AsyncWebServerRequest* r){ handleTestModePage(r); });
    _server.on("/exit", HTTP_GET, [this](AsyncWebServerRequest* r){ handleExit(r); });
    
    _server.on("/api/scan-wifi", HTTP_GET, [this](AsyncWebServerRequest* r){ handleScanWifiApi(r); });
    _server.on("/api/connect-wifi", HTTP_POST, [this](AsyncWebServerRequest* r){ handleConnectWifiApi(r); });
    _server.on("/api/disconnect-wifi", HTTP_POST, [this](AsyncWebServerRequest* r){ handleDisconnectWifiApi(r); });
    _server.on("/api/wifi-status", HTTP_GET, [this](AsyncWebServerRequest* r){ handleWifiStatusApi(r); });
    _server.on("/api/check-ota", HTTP_GET, [this](AsyncWebServerRequest* r){ handleCheckOtaApi(r); });
    _server.on("/api/download-ota", HTTP_POST, [this](AsyncWebServerRequest* r){ handleDownloadOtaApi(r); });
    _server.on("/api/device-status", HTTP_GET, [this](AsyncWebServerRequest* r){ handleDeviceStatusApi(r); });
    _server.on("/api/set-device-id", HTTP_POST, [this](AsyncWebServerRequest* r){ handleSetDeviceIdApi(r); });
    _server.on("/api/run-test", HTTP_POST, [this](AsyncWebServerRequest* r){ handleRunTestApi(r); });

    _server.onNotFound([this](AsyncWebServerRequest* r){ handleNotFound(r); });
}

void WebManager::setupWebSocket() {
    _ws.onEvent([this](auto *s, auto *c, AwsEventType t, void *a, uint8_t *d, size_t l) { 
        onWsEvent(s, c, t, a, d, l); 
    });
    _server.addHandler(&_ws);
}

void WebManager::handleRoot(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->recordWebApiActivity();
    
    // [FIX] 테스트 모드에서 복귀 시 WiFi 재연결을 위해 이 라인을 다시 추가합니다.
    reconnectWifiIfNeeded();
    
    String html = getPageHeader("Nexus Smoke");
    html += "<div class='card'><h3>Wi-Fi Status</h3><p id='home-wifi-status'>Loading...</p></div>";
    html += "<div class='card'><h3>Device Control</h3>"
            "<p><a href='/wifi' class='btn'>Wi-Fi Settings</a></p>"
            "<p><a href='/update' class='btn'>Firmware Update</a></p>"
            "<p><a href='/test' class='btn'>Test Mode</a></p>"
            "<p><a href='/exit' class='btn btn-danger'>Exit Wi-Fi Mode</a></p>"
            "</div>";
    html += getPageFooter(false);
    html += R"rawliteral(
        <script>
            function connectWsForStatus(){
                let ws = new WebSocket("ws://"+window.location.host+"/ws");
                ws.onmessage = e => {
                    try { 
                        const d = JSON.parse(e.data); 
                        if (d.type === "wifi_status_update") { 
                            let s = document.getElementById("home-wifi-status");
                            if (d.connected) {
                                s.innerHTML = "Connected to <b>" + d.ssid + "</b><br>IP Address: " + d.ip;
                            } else {
                                s.textContent = "Not connected. AP Mode is active.";
                            }
                        } 
                    } catch(err) {
                        console.error("Home WS Parse Error:", err);
                    }
                };
                ws.onclose = () => { setTimeout(connectWsForStatus, 2000); };
            }
            window.onload = connectWsForStatus;
        </script>
    )rawliteral";
    request->send(200, "text/html; charset=UTF-8", html);
}

void WebManager::handleWifiConfigPage(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->recordWebApiActivity();
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
                <select id="ssid-select" class="form-control">
                    <option value="">-- Scan to select a network --</option>
                </select>
            </div>
            <div style="display: flex; justify-content: center; margin-top: 20px; gap: 10px;">
                <button id="scan-btn" class="btn" onclick="scanWifi()">Rescan</button>
            </div>
            <div class="form-group" style="text-align: center;">
                <label for="password-input">Password:</label>
                <input type="password" id="password-input" class="form-control">
            </div>
            <!-- Unified Connect/Disconnect button -->
            <button id="action-btn" class="btn" onclick="handleConnectDisconnect()">Connect</button> 
        </div>
        <script>
            const scanBtn = document.getElementById("scan-btn");
            const actionBtn = document.getElementById("action-btn"); 
            const ssidSelect = document.getElementById("ssid-select");
            const passwordInput = document.getElementById("password-input");
            const connStatusEl = document.getElementById("conn-status");
            let ws, currentSsid = '';
            
            // WebSocket 연결 설정
            function connectWs() {
                ws = new WebSocket(`ws://${location.host}/ws`);
                ws.onopen = () => { 
                    console.log('WebSocket Connected'); 
                    fetchStatus(); 
                };
                ws.onclose = () => { 
                    console.log('WebSocket Disconnected, reconnecting...'); 
                    setTimeout(connectWs, 2000); 
                };
                ws.onmessage = evt => {
                    try {
                        const data = JSON.parse(evt.data);
                        if (data.type === "wifi_status_update") handleWifiStatus(data);
                        if (data.type === "scan_result") handleScanResult(data);
                    } catch (e) { 
                        console.error("WS Parse Error:", e); 
                    }
                };
            }

            // API에서 현재 WiFi 상태 가져오기
            function fetchStatus() { 
                fetch("/api/wifi-status")
                    .then(response => response.json())
                    .then(data => handleWifiStatus(data))
                    .catch(err => console.error('Error fetching status:', err));
            }

            // 연결/연결 해제 버튼 클릭 처리
            function handleConnectDisconnect() {
                if (actionBtn.textContent === "Connect") {
                    connectWifi();
                } else if (actionBtn.textContent === "Disconnect") {
                    disconnectWifi();
                }
            }

            // WiFi 상태에 따라 UI 업데이트
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

                    if (data.status === "connected") { 
                        showMessage("WiFi connection successful!", 'success'); 
                    }
                } else { // 연결되지 않음
                    connStatusEl.textContent = "Not Connected";
                    actionBtn.textContent = "Connect";
                    actionBtn.classList.add('btn');
                    
                    switch (data.status) {
                        case "connecting":
                            showMessage("Connecting to selected WiFi...", 'info', 0);
                            actionBtn.disabled = true; 
                            scanBtn.disabled = true;
                            passwordInput.disabled = true;
                            ssidSelect.disabled = true;
                            break;
                        case "failed":
                            actionBtn.disabled = false; 
                            scanBtn.disabled = false;
                            passwordInput.disabled = false;
                            ssidSelect.disabled = false;
                            if (data.reason === 15 || data.reason === 2 || data.reason === 8) {
                                showMessage("Connection failed. Please check your password.", 'error');
                            } else if (data.reason === 201) {
                                showMessage("Connection failed. The network is out of range or not found.", 'error');
                            } else {
                                showMessage(`Connection failed. Environment may be unstable. (Reason: ${data.reason})`, 'error');
                            }
                            break;
                        case "disconnected":
                            showMessage("Disconnected from WiFi.", 'info'); 
                            break;
                        default:
                            break;
                    }
                }
            }

            // WiFi 스캔 시작 함수
            function scanWifi() {
                if (scanBtn.disabled) return;
                scanBtn.disabled = true;
                ssidSelect.innerHTML = "<option>Scanning...</option>";
                showMessage("Scanning for WiFi networks...", "info");
                fetch("/api/scan-wifi").catch(error => {
                    showMessage("Failed to start scan.", "error");
                    scanBtn.disabled = false;
                });
            }

            // 스캔 결과 처리
            function handleScanResult(data) {
                ssidSelect.innerHTML = "<option value=''>-- Select a Network --</option>";
                if (data.networks && data.networks.length > 0) {
                    data.networks.slice(0, 20).forEach(net => {
                        const lockIcon = net.encrypted ? "🔒" : " ";
                        const option = new Option(`${lockIcon} ${net.ssid} (${net.rssi} dBm)`, net.ssid);
                        ssidSelect.add(option);
                    });
                    showMessage("Scan complete. Select a network.", "success");
                } else {
                    ssidSelect.innerHTML = "<option>No WiFi networks found.</option>";
                    showMessage("No WiFi networks found.", "error");
                }
                scanBtn.disabled = false;
            }
            
            // 선택된 WiFi에 연결하는 함수
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
                fetch("/api/connect-wifi", {
                    method: "POST",
                    headers: { "Content-Type": "application/x-www-form-urlencoded" },
                    body: `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`
                }).catch(err => {
                    showMessage("Failed to send connection request.", "error");
                    actionBtn.disabled = false;
                    scanBtn.disabled = false;
                    passwordInput.disabled = false;
                    ssidSelect.disabled = false;
                });
            }

            // WiFi 연결 해제 함수
            function disconnectWifi() {
                if (!window.confirm(`Are you sure you want to disconnect from ${currentSsid}? The saved password for this network will be removed.`)) {
                    return;
                }
                actionBtn.disabled = true;
                showMessage('Disconnecting...', 'info', 0);
                fetch("/api/disconnect-wifi", {
                    method: "POST",
                    headers: { "Content-Type": "application/x-www-form-urlencoded" },
                    body: `ssid=${encodeURIComponent(currentSsid)}`
                }).then(() => {
                    passwordInput.value = '';
                }).catch(err => {
                    showMessage('Failed to send disconnect request.', 'error');
                });
            }

            // 창 로드 시 WebSocket 초기화 및 초기 스캔/상태 확인
            window.onload = () => {
                connectWs();
                fetchStatus(); 
                setTimeout(scanWifi, 500); 
            };
        </script>
    )rawliteral";
    html += getPageFooter(true);
    request->send(200, "text/html; charset=UTF-8", html);
}

void WebManager::handleFirmwareUpdatePage(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->recordWebApiActivity();
    String html = getPageHeader("Firmware Update");
    html += R"rawliteral(
        <div class='card'>
            <p>Current Version: <b id='current-v'>-</b><br>Latest on Server: <b id='latest-v'>-</b></p>
            <div id='update-info'>
                 <div id='changelog' class='changelog'></div>
                 <p id='update-status'></p>
            </div>
            <div class="form-group" style="text-align: center;"> <!-- 버튼 중앙 정렬 -->
                <button id='update-btn' class='btn hidden' onclick='downloadUpdate()'>Update</button> <!-- 버튼 텍스트 변경 -->
                <div id='download-progress' class='hidden' style='margin-top: 10px; display: flex; flex-direction: column; align-items: center;'>
                    <span id='progress-text' style='font-weight: bold;'>0%</span>
                    <div class='progress-bar'>
                        <div class='progress-bar-inner' id='progress-bar-inner'></div>
                    </div>
                </div>
                <p id='download-notice' class='hidden notice'>Firmware will be downloaded now. The update will be applied when you exit Wi-Fi mode.</p>
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
            let ws;

            function connectWs() {
                ws = new WebSocket("ws://"+window.location.host+"/ws");
                ws.onmessage = e => {
                    try {
                        const d = JSON.parse(e.data);
                        if (d.type === "ota_status") updateOtaUi(d);
                        if (d.type === "ota_progress") updateProgressText(d.progress);
                        if (d.type === "ota_result") handleOtaResult(d);
                        if (d.type === "wifi_status_update") {
                            console.log("WiFi status changed, re-checking OTA status.");
                            fetch("/api/check-ota");
                        }
                    } catch(err) { console.error("OTA WS Error:", err); }
                };
                ws.onopen = () => fetch("/api/check-ota");
                ws.onclose = () => setTimeout(connectWs, 2000);
            }

            function updateOtaUi(d) {
                document.getElementById("current-v").textContent = d.current_version;
                document.getElementById("latest-v").textContent = d.latest_version;
                
                if (!d.internet_ok) {
                     updateStatus.innerHTML = "<span class='message-error'>Server access is required for updates. Please connect to a Wi-Fi network with internet access first.</span>";
                     changelogEl.textContent = "Cannot fetch changelog without an internet connection.";
                     updateBtn.classList.add("hidden");
                     downloadProgressDiv.classList.add('hidden'); 
                     return;
                }

                changelogEl.textContent = d.changelog || "Could not retrieve changelog.";

                if (d.update_available) {
                    updateStatus.innerHTML = "<b style='color:green;'>Update available!</b>";
                    updateBtn.classList.remove("hidden");
                    updateBtn.disabled = false;
                    downloadProgressDiv.classList.add('hidden'); 
                } else {
                    updateStatus.textContent = "You are on the latest version.";
                    updateBtn.classList.add("hidden");
                    downloadProgressDiv.classList.add('hidden'); 
                }
            }
            
            function downloadUpdate() {
                const customConfirm = (msg, onConfirm, onCancel) => {
                     if (window.confirm(msg)) { 
                        onConfirm();
                    } else {
                        onCancel();
                    }
                };
                customConfirm("Start download? The device may be unresponsive during download. The update will be applied on exit.", () => {
                    updateBtn.disabled = true;
                    downloadNotice.classList.remove("hidden");
                    downloadProgressDiv.classList.remove('hidden'); 
                    updateProgressText(0); 
                    fetch("/api/download-ota", { method: "POST" });
                }, () => {
                    // 사용자가 취소함
                });
            }

            function updateProgressText(progress) {
                progressText.textContent = `${progress}%`;
                progressBarInner.style.width = `${progress}%`;
            }
            
            function handleOtaResult(d) {
                showMessage(d.msg, d.success ? 'success' : 'error'); // showMessage 사용
                if (d.success) {
                    updateBtn.textContent = "Update Complete";
                    updateBtn.classList.remove("btn");
                    updateBtn.classList.add('btn-success');
                    downloadProgressDiv.classList.add('hidden'); 
                } else {
                    updateBtn.textContent = "Update"; 
                    updateBtn.disabled = false;
                    downloadProgressDiv.classList.add('hidden'); 
                }
            }

            window.onload = connectWs;
        </script>
    )rawliteral";
    html += getPageFooter(true);
    request->send(200, "text/html; charset=UTF-8", html);
}

void WebManager::handleTestModePage(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->switchToMode(DeviceMode::MODE_TEST);

    if (WiFi.status() == WL_CONNECTED) {
        Log::Info(PSTR("WEB: Entering Test Mode. Temporarily disconnecting Wi-Fi."));
        _disconnectedForTestSsid = WiFi.SSID();
        _reconnectOnExitTest = true;
        WiFi.disconnect(true);
    }

    String html = getPageHeader("Test Mode");
    html += R"rawliteral(
        <div class='card'>
            <h3>Device Settings</h3>
            <table style='width:100%; text-align:left; border-spacing: 0 10px; border-collapse: separate;'>
              <tr>
                <td style='width:140px;'><label for='dev-id'>Device ID :</label></td>
                <td>
                  <div style='display:flex; align-items:center;'>
                    <input type='number' id='dev-id' min='1' max='10' style='width: 80px; margin:0;'>
                    <button onclick='saveId()' class='btn' style='padding:5px 10px; min-width:auto; margin-left: 10px;'>Save</button>
                  </div>
                </td>
              </tr>
              <tr>
                <td><label for='delay-s'>Delay Timer (s) :</label></td>
                <td><input type='number' id='delay-s' placeholder='Delay' step='0.1' style='width: 80px;'></td>
              </tr>
              <tr>
                <td><label for='play-s'>Play Timer (s) :</label></td>
                <td><input type='number' id='play-s' placeholder='Play' step='0.1' style='width: 80px;'></td>
              </tr>
            </table>
            <p><button onclick='runTest()' id='run-test-btn' class='btn'>Run Manual Test</button></p>
        </div>
        <div class='card'>
            <h3>Live Log (<a href='javascript:void(0);' onclick='document.getElementById("log").innerHTML=""'>Clear</a>)</h3>
            <div id='log' style='height:300px;overflow-y:scroll;border:1px solid #ccc;text-align:left;padding:5px;font-family:monospace;font-size:0.9em;background:#333;color:#eee;white-space:pre-wrap;'></div>
            <p style='margin-top:15px; font-weight: bold; color: #d9534f;'>
                This mode does not support connection with the transmitter.<br>Communication will be enabled when you exit this mode.
            </p>
        </div>
        <script>
            let log=document.getElementById("log");
            let ws;

            function showMessage(text, type = 'info', duration = 3000) {
                console.log(`[${type}] ${text}`);
            }

            function getStatus(){
                fetch("/api/device-status")
                .then(r=>r.json())
                .then(d=>{
                    document.getElementById("dev-id").value = d.device_id;
                    document.getElementById("delay-s").value = d.test_delay_ms / 1000.0;
                    document.getElementById("play-s").value = d.test_play_ms / 1000.0;
                })
                .catch(err => showMessage('Failed to fetch device status.', 'error'));
            }
            
            function saveId(){
                fetch("/api/set-device-id",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"id="+document.getElementById("dev-id").value})
                .then(response => {
                    if (response.ok) showMessage('ID Saved!', 'success');
                    else showMessage('Failed to save ID.', 'error');
                })
                .catch(err => showMessage('Failed to save ID.', 'error'));
            }

            function runTest(){
                let btn = document.getElementById('run-test-btn');
                btn.disabled = true;
                btn.textContent = 'Running...';
                
                let delayMs = parseFloat(document.getElementById('delay-s').value) * 1000;
                let playMs = parseFloat(document.getElementById('play-s').value) * 1000;

                let formData = new URLSearchParams();
                formData.append('delay', delayMs);
                formData.append('play', playMs);

                fetch("/api/run-test",{
                    method:"POST",
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: formData
                }).catch(err => {
                    showMessage('Failed to start test.', 'error');
                    btn.disabled = false;
                    btn.textContent = 'Run Manual Test';
                });
            }

            function connectWs() {
                ws = new WebSocket("ws://"+window.location.host+"/ws");
                ws.onmessage=e=>{
                    try{
                        let d=JSON.parse(e.data);
                        if(d.type==="log"){
                            log.innerHTML+=`<div style="color:#fff;">[${(d.ts / 1000).toFixed(1)}s] ${d.msg}</div>`;
                            log.scrollTop=log.scrollHeight;
                        }
                        if(d.type==="test_completed"){
                             let btn = document.getElementById('run-test-btn');
                             btn.disabled = false;
                             btn.textContent = 'Run Manual Test';
                             log.innerHTML+=`<div style="color:#fff;">[${(Date.now() / 1000).toFixed(1)}s] Test Completed.</div>`;
                        }
                    }catch(e){
                        console.error("Test WS Parse Error:", e);
                    }
                };

                ws.onclose = () => {
                    console.log("Test WS closed, reconnecting...");
                    setTimeout(connectWs, 2000);
                };
                ws.onerror = (err) => {
                    console.error("Test WS Error:", err);
                    ws.close();
                };
            }
            window.onload = () => {
                getStatus();
                connectWs();
            };
        </script>
    )rawliteral";
    html += getPageFooter(true);
    request->send(200, "text/html; charset=UTF-8", html);
}

void WebManager::handleExit(AsyncWebServerRequest* request) {
    String html = getPageHeader("Exiting Wi-Fi Mode");
    html += "<p>The device will now return to normal operation. You can close this window.</p>";
    if (_otaUpdateDownloaded.load()) {
        html += "<p style='color:blue;font-weight:bold;'>An update was downloaded and will be applied on reboot.</p>";
    }
    html += getPageFooter(false);
    request->send(200, "text/html; charset=UTF-8", html);
    delay(100);
    if (_modeManager) _modeManager->exitWifiMode();
}

void WebManager::handleNotFound(AsyncWebServerRequest* request) { 
    request->send(404, "text/plain", "Not Found"); 
}

void WebManager::handleScanWifiApi(AsyncWebServerRequest* request) {
    if (_isScanningWifi.load()) {
        request->send(429, "application/json", "{\"status\":\"busy\", \"message\":\"Scan already in progress.\"}");
        return;
    }
    _isScanningWifi = true;
    xTaskCreate(wifiScanTask, "wifiScanTask", 4096, this, 5, NULL);
    request->send(202, "application/json", "{\"status\":\"accepted\", \"message\":\"Scan started.\"}");
}

void WebManager::handleConnectWifiApi(AsyncWebServerRequest* request) {
    if (_isConnectingWifi.load()) {
        request->send(429, "application/json", "{\"error\":\"Connection already in progress\"}");
        return;
    }
    if (!request->hasParam("ssid", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing SSID\"}");
        return;
    }

    String ssid = request->getParam("ssid", true)->value();
    String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";

    Log::Info(PSTR("WEB: Received connect request for SSID: %s"), ssid.c_str());

    _isConnectingWifi = true;
    _wifiConnectStartMillis = millis();

    Utils::saveWifiCredential(ssid, password);
    
    broadcastWifiStatus("connecting");
    
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    request->send(202, "application/json", "{\"status\":\"connection_attempt_started\"}");
}

void WebManager::handleWifiStatusApi(AsyncWebServerRequest* request) {
    JsonDocument doc;
    getWifiStatusJson(doc);
    if(request) {
        String output; 
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    }
}

void WebManager::handleCheckOtaApi(AsyncWebServerRequest* request) {
    xTaskCreate(otaCheckVersionTask, "otaCheckTask", 4096, this, 5, NULL); // Create task to check for OTA updates
    request->send(200, "application/json", "{\"status\":\"checking\"}");
}

void WebManager::handleDownloadOtaApi(AsyncWebServerRequest* request) {
    xTaskCreate(otaDownloadTask, "otaDownloadTask", 10240, this, 2, NULL); // Create task to download OTA firmware
    request->send(200, "application/json", "{\"status\":\"download_started\"}");
}

void WebManager::handleDeviceStatusApi(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["device_id"] = Utils::loadDeviceId();
    doc["test_delay_ms"] = Utils::loadTestDelay();
    doc["test_play_ms"] = Utils::loadTestPlay();
    String output; serializeJson(doc, output);
    request->send(200, "application/json", output);
}

void WebManager::handleSetDeviceIdApi(AsyncWebServerRequest* request) {
    if (request->hasParam("id", true)) {
        _modeManager->updateDeviceId(request->getParam("id", true)->value().toInt(), true); // Update device ID via ModeManager
    }
    request->send(200); // Send success response
}

void WebManager::handleRunTestApi(AsyncWebServerRequest* request) {
    uint32_t delayMs = Utils::loadTestDelay(); // Get default delay
    uint32_t playMs = Utils::loadTestPlay();   // Get default play duration

    // Override with values from request if provided
    if (request->hasParam("delay", true)) {
        delayMs = request->getParam("delay", true)->value().toInt();
    }
    if (request->hasParam("play", true)) {
        playMs = request->getParam("play", true)->value().toInt();
    }

    if (_modeManager) {
        _modeManager->triggerManualRun(delayMs, playMs); // Trigger manual test run in ModeManager
    }
    request->send(200, "application/json", "{\"status\":\"started\"}");
}

void WebManager::handleDisconnectWifiApi(AsyncWebServerRequest* request) {
    Log::Info(PSTR("WEB: Received disconnect request."));
    if (request->hasParam("ssid", true)) {
        String ssidToForget = request->getParam("ssid", true)->value();
        Utils::removeWifiCredential(ssidToForget); // Remove credential from NVS
        Log::Info(PSTR("WEB: Wi-Fi credential for %s was forgotten."), ssidToForget.c_str());
    }
    
    WiFi.disconnect(true, true); // Disconnect from WiFi and erase credentials
    request->send(200, "application/json", "{\"status\":\"disconnected\"}");
}

// --- Event Handlers & Helpers ---

void WebManager::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (!_instance) return;
    
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            Serial.println("WiFi client started");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_STOP:
            Serial.println("WiFi client stopped");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("Connected to WiFi network");
            _instance->_lastDisconnectReason = 0;
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.printf("Disconnected from WiFi network. Reason: %d\n", info.wifi_sta_disconnected.reason);
            _instance->_lastDisconnectReason = info.wifi_sta_disconnected.reason;

            if (_instance->_isConnectingWifi.load()) {
                int reason = info.wifi_sta_disconnected.reason;
                if (reason == 15 || reason == 201 || reason == 2 || reason == 8) {
                    Log::Warn(PSTR("WEB: WiFi connection failed with definitive reason: %d. Broadcasting failure."), reason);
                    _instance->_isConnectingWifi = false;
                    _instance->broadcastWifiStatus("failed", reason);
                } else {
                    Log::Debug(PSTR("WEB: Transient disconnect during connection attempt (Reason: %d). Waiting for final status."), reason);
                }
            } else {
                _instance->broadcastWifiStatus("disconnected", info.wifi_sta_disconnected.reason);
            }
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("Got IP address: %s\n", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
            if (_instance) {
                if (_instance->_isConnectingWifi.load()) {
                    _instance->_isConnectingWifi = false;
                }
                _instance->_lastDisconnectReason = 0;
                _instance->broadcastWifiStatus("connected");
            }
            break;
            
        case ARDUINO_EVENT_WIFI_AP_START:
            Serial.println("AP started");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STOP:
            Serial.println("AP stopped");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("Client connected to AP");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("Client disconnected from AP");
            break;
        default:
            break;
    }
}

void WebManager::onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (_modeManager) _modeManager->recordWebApiActivity();
    if (type == WS_EVT_CONNECT) {
        Log::Debug(PSTR("WEB: WebSocket client #%u connected from %s"), client->id(), client->remoteIP().toString().c_str());
        
        JsonDocument doc;
        getWifiStatusJson(doc);
        doc["type"] = "wifi_status_update";
        
        String output;
        serializeJson(doc, output);
        client->text(output);
        
        broadcastOtaStatus();
    } else if (type == WS_EVT_DISCONNECT) {
        Log::Debug(PSTR("WEB: WebSocket client #%u disconnected."), client->id());
    }
}

void WebManager::wifiScanTask(void* pvParameters) {
    WebManager* self = static_cast<WebManager*>(pvParameters);
    Log::Info(PSTR("WEB: Starting WiFi scan..."));

    int n = WiFi.scanNetworks();
    
    JsonDocument doc;
    doc["type"] = "scan_result";
    JsonArray networksArray = doc["networks"].to<JsonArray>();

    if (n > 0) {
        Log::Info(PSTR("WEB: Scan found %d networks."), n);
        std::vector<std::tuple<String, int, wifi_auth_mode_t>> found_networks;
        for (int i = 0; i < n; ++i) {
            found_networks.push_back(std::make_tuple(
                WiFi.SSID(i),
                WiFi.RSSI(i),
                WiFi.encryptionType(i)
            ));
        }
        
        std::sort(found_networks.begin(), found_networks.end(),
            [](const auto& a, const auto& b) {
                return std::get<1>(a) > std::get<1>(b);
            });

        for (const auto& net : found_networks) {
            JsonObject netObj = networksArray.add<JsonObject>();
            netObj["ssid"] = std::get<0>(net);
            netObj["rssi"] = std::get<1>(net);
            netObj["encrypted"] = (std::get<2>(net) != WIFI_AUTH_OPEN);
        }
    } else {
        Log::Warn(PSTR("WEB: Scan failed or no networks found. Result: %d"), n);
    }

    self->broadcastJson(doc);
    WiFi.scanDelete();
    
    self->_isScanningWifi = false;
    vTaskDelete(NULL);
}

void WebManager::getWifiStatusJson(JsonDocument& doc) {
    doc["connected"] = WiFi.status() == WL_CONNECTED;
    if (doc["connected"]) {
        doc["ssid"] = WiFi.SSID();
        doc["rssi"] = WiFi.RSSI();
        doc["ip"] = WiFi.localIP().toString();
    }
    doc["scanning"] = _isScanningWifi.load();
    doc["connecting"] = _isConnectingWifi.load();
    if (_lastDisconnectReason != 0) {
        doc["last_disconnect_reason"] = _lastDisconnectReason;
    }
}

void WebManager::broadcastWifiStatus(const char* status, int reason) {
    JsonDocument doc;
    getWifiStatusJson(doc);
    
    doc["type"] = "wifi_status_update";
    doc["status"] = status;
    if (reason != 0) {
        doc["reason"] = reason;
    }
    broadcastJson(doc);
}

void WebManager::broadcastOtaStatus() {
    JsonDocument doc;
    bool isConnected = (WiFi.status() == WL_CONNECTED);

    doc["type"] = "ota_status";
    
    doc["internet_ok"] = isConnected;
    doc["current_version"] = _currentFirmwareVersion;
    doc["latest_version"] = isConnected ? _latestOtaVersion : "N/A";
    doc["update_available"] = isConnected ? _otaUpdateAvailable : false;
    
    doc["changelog"] = isConnected ? _otaChangeLog : "Connect to Wi-Fi to check for updates.";

    broadcastJson(doc);
}

void WebManager::broadcastOtaProgress(int progress) {
    JsonDocument doc;
    doc["type"] = "ota_progress";
    doc["progress"] = progress;
    broadcastJson(doc);
}

void WebManager::broadcastJson(const JsonDocument& doc) {
    String output;
    serializeJson(doc, output);
    _ws.textAll(output); // Send JSON string to all connected WebSocket clients
}

void WebManager::otaCheckVersionTask(void* pvParameters) {
    WebManager* self = static_cast<WebManager*>(pvParameters);
    self->fetchOtaVersionInfo(); // Fetch version info from server
    self->broadcastOtaStatus(); // Broadcast status to UI
    vTaskDelete(NULL); // Delete the task
}

void WebManager::otaDownloadTask(void* pvParameters) {
    WebManager* self = static_cast<WebManager*>(pvParameters);
    self->downloadAndApplyOta(); // Download and apply OTA
    vTaskDelete(NULL); // Delete the task
}

bool WebManager::fetchOtaVersionInfo() {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    
    http.begin(client, OTA_VERSION_URL);
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getStream()) == DeserializationError::Ok) {
            xSemaphoreTake(_otaDataMutex, portMAX_DELAY);
            _latestOtaVersion = doc["version"].as<String>();
            _otaChangeLog = doc["changelog"].as<String>();
            _otaUpdateAvailable = isVersionNewer(_latestOtaVersion, _currentFirmwareVersion);
            xSemaphoreGive(_otaDataMutex);
            http.end();
            return true;
        }
    }
    http.end();
    return false;
}

void WebManager::downloadAndApplyOta() {
    esp_task_wdt_add(NULL);

    JsonDocument doc;
    doc["type"] = "ota_result";

    if (WiFi.status() != WL_CONNECTED) {
        doc["success"] = false;
        doc["message"] = "OTA Failed: No Internet Connection.";
    } else {
        HTTPClient http;
        WiFiClientSecure client;
        client.setInsecure();
        
        http.begin(client, OTA_FIRMWARE_URL);
        int httpCode = http.GET();
        
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            if (contentLength > 0 && Update.begin(contentLength)) {
                Log::Info("OTA: Starting download. Size: %d bytes.", contentLength);
                WiFiClient *stream = http.getStreamPtr();
                size_t written = 0;
                int lastProgress = -1;
                uint8_t buff[1024] = { 0 };

                while (http.connected() && (written < contentLength)) {
                    esp_task_wdt_reset();
                    size_t len = stream->readBytes(buff, sizeof(buff));
                    if (len > 0) {
                        Update.write(buff, len);
                        written += len;
                        int progress = (int)(((float)written / (float)contentLength) * 100);
                        if (progress > lastProgress) {
                            broadcastOtaProgress(progress);
                            lastProgress = progress;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(1));
                }

                if (written == contentLength && Update.end(true)) {
                    _otaUpdateDownloaded = true;
                    if (_modeManager) _modeManager->setUpdateDownloaded(true);
                    doc["success"] = true;
                    doc["message"] = "Download complete! Update will be applied on exit.";
                    Log::Info("OTA: Download successful.");
                } else {
                    doc["success"] = false;
                    doc["message"] = "Update failed: " + String(Update.errorString());
                    Update.abort();
                }
            } else {
                doc["success"] = false;
                doc["message"] = "Not enough space or invalid content length. Error: " + String(Update.getError());
            }
        } else {
            doc["success"] = false;
            doc["message"] = "Failed to download. HTTP Error: " + String(httpCode);
        }
        http.end();
    }
    
    broadcastJson(doc);
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

void WebManager::setupLogBroadcaster() {
    Log::setWebSocketLogSender([this](const char* level, const String& msg) {
        if (_isServerRunning.load() && strcmp(level, "TEST") == 0) {
            JsonDocument doc;
            doc["type"] = "log";
            doc["ts"] = millis();
            doc["msg"] = msg;

            broadcastJson(doc);
        }
    });
}

String WebManager::getPageHeader(const String& title) {
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
            max-width: 550px;
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
        
        /* Floating Message Toast */
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

String WebManager::getPageFooter(bool showHomeButton) {
    String html; 
    if (showHomeButton) html += F("<p style='margin-top:25px;'><a href='/' class='btn'>Back to Home</a></p>");
    html += F("</div></body></html>"); 
    return html;
}
