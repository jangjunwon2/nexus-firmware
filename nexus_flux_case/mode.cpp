#include "mode.h"
#include "hardware.h"
#include "comm.h"
#include "web.h"
#include <algorithm>
#include <esp_sleep.h>
#include <esp_now.h>
#include <WiFi.h>
#include "driver/gpio.h"

ModeManager::ModeManager(HardwareManager* hwManager, CommManager* commManager, WebManager* webManager)
    : _hwManager(hwManager), _commManager(commManager), _webManager(webManager),
      _currentMode(DeviceMode::MODE_BOOT),
      _deviceId(DEFAULT_DEVICE_ID),
      _currentCommandId(0),
      _planStepCount(0), _currentStepIndex(-1),
      _idSetState(IdSetState::IDLE),
      _temporaryId(0),
      _idSetLastInputTime(0),
      _isManualOperationActive(false),
      _isDelayPhase(false), _delayPhaseEndTime(0), _playPhaseEndTime(0),
      _lastWebApiActivityTime(0), _updateDownloaded(false),
      _idBlinkPatternStarted(false),
      _previousDeviceId(DEFAULT_DEVICE_ID),
      _pairingStartTime(0)
{
    _modeSwitchMutex = xSemaphoreCreateMutex();
    _playSequenceMutex = xSemaphoreCreateMutex();
}

void ModeManager::begin() {
    if (!_hwManager || !_commManager || !_webManager) {
        Log::Error(PSTR("MODE: Critical manager dependency is missing! Halting."));
        while(1) { vTaskDelay(1000); }
    }

    _deviceId = Utils::loadDeviceId().toInt();
    if (_deviceId < MIN_DEVICE_ID || _deviceId > MAX_DEVICE_ID) {
        _deviceId = DEFAULT_DEVICE_ID;
        Utils::saveDeviceId(String(_deviceId));
    }

    _commManager->updateMyDeviceId(_deviceId);
    Log::Info(PSTR("MODE: ModeManager initialized. Device ID is %d."), _deviceId);
    _hwManager->setLedPattern(LedPatternType::LED_BOOT_SUCCESS);
}

void ModeManager::switchToMode(DeviceMode newMode, bool forceSwitch) {
    if (xSemaphoreTake(_modeSwitchMutex, (TickType_t)0) != pdTRUE) return;
    if (!forceSwitch && _currentMode == newMode) { xSemaphoreGive(_modeSwitchMutex); return; }

    bool isStayingInWebUi = (_currentMode == DeviceMode::MODE_WIFI && newMode == DeviceMode::MODE_TEST) ||
                            (_currentMode == DeviceMode::MODE_TEST && newMode == DeviceMode::MODE_WIFI);

    if (!isStayingInWebUi) exitModeLogic(_currentMode);
    _currentMode = newMode;
    if (!isStayingInWebUi) enterModeLogic(_currentMode);
    xSemaphoreGive(_modeSwitchMutex);
}

void ModeManager::handleEspNowCommand(const uint8_t* senderMac, const Comm::CommPacket& pkt, uint32_t rxTime) {
    if (!_commManager || !senderMac) return;

    // MachineType 필터링 (ALL 이거나 내 타입일 때만)
    if (pkt.targetMachineType != TYPE_ALL && pkt.targetMachineType != MY_MACHINE_TYPE) {
        Log::Debug(PSTR("COMM: Ignored packet for different machine type: %d"), pkt.targetMachineType);
        return; 
    }
    
    if (_currentMode == DeviceMode::MODE_ID_SET) {
        if (pkt.packetType == Comm::RTT_REQUEST) _commManager->sendAck(senderMac, pkt, rxTime);
        return;
    }

    if (pkt.packetType == Comm::RTT_REQUEST) {
        _commManager->sendAck(senderMac, pkt, rxTime);
    } else if (pkt.packetType == Comm::FINAL_COMMAND) {
        xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
        if (_currentStepIndex != -1) stopExecutionPlan();
        startExecutionPlan(pkt);
        xSemaphoreGive(_playSequenceMutex);
        _commManager->sendAck(senderMac, pkt, rxTime);
    }
}

void ModeManager::enterModeLogic(DeviceMode mode) {
    switch (mode) {
        case DeviceMode::MODE_NORMAL:
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_OFF);
            break;
        case DeviceMode::MODE_ID_SET:
            _previousDeviceId = _deviceId;
            _temporaryId = 0;
            _idSetState = IdSetState::AWAITING_INPUT;
            _idSetLastInputTime = millis();
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ID_SET_ENTER);
            break;
        case DeviceMode::MODE_ID_BLINK:
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
            break;
            
        // [NEW] 페어링 모드 진입 시
        case DeviceMode::MODE_PAIRING:
            _pairingStartTime = millis();
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_PAIRING); 
            if (_commManager) _commManager->setPairingMode(true); 
            Log::Info(PSTR("MODE: Entered Pairing Mode. Press 'SYNC(MAIN)' on Transmitter..."));
            break;

        case DeviceMode::MODE_WIFI:
            xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
            stopExecutionPlan();
            xSemaphoreGive(_playSequenceMutex);
            if (_hwManager) _hwManager->shutdownOutputs();
            _hwManager->setLedPattern(LedPatternType::LED_WIFI_MODE_TOGGLE);
            _lastWebApiActivityTime = millis();
            attemptAutoConnection();
            if (_webManager) _webManager->startServer();
            break;
        case DeviceMode::MODE_TEST:
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ON);
            break;
        case DeviceMode::MODE_EXIT_WIFI:
            if (_webManager && _webManager->isServerRunning() && _updateDownloaded) {
                applyUpdateAndReboot();
                return;
            }
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_WIFI_MODE_TOGGLE);
            if (_webManager && _webManager->isServerRunning()) _webManager->stopServer();
            WiFi.softAPdisconnect(true);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (_commManager) _commManager->reinitForEspNow();
            if (_hwManager) _hwManager->shutdownOutputs();
            break;
        case DeviceMode::MODE_ERROR:
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ERROR);
            break;
        default: break;
    }
}

void ModeManager::attemptAutoConnection() {
    Log::Info(PSTR("MODE: Attempting Wi-Fi auto-connection..."));
    String credsJson = Utils::loadWifiCredentials();
    if (credsJson == "[]") return;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks();
    if (n == 0) return;

    JsonDocument doc;
    deserializeJson(doc, credsJson);
    JsonArray savedCreds = doc.as<JsonArray>();

    String bestSsid = "";
    String bestPass = "";
    int8_t maxRssi = -100;

    for (int i = 0; i < n; ++i) {
        for (JsonObject saved : savedCreds) {
            if (WiFi.SSID(i) == saved["ssid"].as<String>()) {
                if (WiFi.RSSI(i) > maxRssi) {
                    maxRssi = WiFi.RSSI(i);
                    bestSsid = saved["ssid"].as<String>();
                    bestPass = saved["pass"].as<String>();
                }
                break;
            }
        }
    }
    WiFi.scanDelete();

    if (bestSsid != "") {
        Log::Info(PSTR("MODE: Connecting to %s..."), bestSsid.c_str());
        WiFi.begin(bestSsid.c_str(), bestPass.c_str());
    }
}

void ModeManager::exitModeLogic(DeviceMode mode) {
    if (mode == DeviceMode::MODE_ID_SET) _idSetState = IdSetState::IDLE;
    // [NEW] 페어링 종료 시 통신매니저도 원복
    if (mode == DeviceMode::MODE_PAIRING && _commManager) _commManager->setPairingMode(false);
}

void ModeManager::applyUpdateAndReboot() {
    if (_webManager) _webManager->performUpdateAndReboot();
}

void ModeManager::update() {
    if(_hwManager) handleButtonEvent(_hwManager->getButtonEvent());

    xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
    if (_currentStepIndex != -1) {
        updateExecutionPlan();
    }
    xSemaphoreGive(_playSequenceMutex);

    switch (_currentMode) {
        case DeviceMode::MODE_NORMAL:   updateModeNormal();  break;
        case DeviceMode::MODE_ID_BLINK: updateModeIdBlink(); break;
        case DeviceMode::MODE_ID_SET:   updateModeIdSet();   break;
        case DeviceMode::MODE_WIFI:
        case DeviceMode::MODE_TEST:     updateModeWifi();    break;
        case DeviceMode::MODE_EXIT_WIFI: updateModeExitWifi(); break;
        case DeviceMode::MODE_PAIRING:  updateModePairing(); break; // [NEW] 
        default: break;
    }
}

void ModeManager::handleButtonEvent(ButtonEventType event) {
    if (event == ButtonEventType::NO_EVENT) return;

    if (event == ButtonEventType::BOTH_BUTTONS_LONG_PRESS) {
        xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
        stopManualOperation();
        stopExecutionPlan();
        xSemaphoreGive(_playSequenceMutex);

        if (_currentMode == DeviceMode::MODE_WIFI || _currentMode == DeviceMode::MODE_TEST) {
            switchToMode(DeviceMode::MODE_EXIT_WIFI);
        } else {
            switchToMode(DeviceMode::MODE_WIFI);
        }
        return;
    }

    if (_currentMode == DeviceMode::MODE_WIFI || _currentMode == DeviceMode::MODE_TEST) {
        if (event == ButtonEventType::ID_BUTTON_SHORT_PRESS) {
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
        }
        xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
        if (event == ButtonEventType::EXEC_BUTTON_PRESS) {
            if (_currentStepIndex == -1 && !_isManualOperationActive) startManualOperation();
        } else if (event == ButtonEventType::EXEC_BUTTON_RELEASE) {
            if (_isManualOperationActive) stopManualOperation();
        }
        xSemaphoreGive(_playSequenceMutex);
        return;
    }

    if (_currentMode == DeviceMode::MODE_ID_SET) {
        _idSetLastInputTime = millis();
        if (event == ButtonEventType::ID_BUTTON_SHORT_PRESS) incrementTemporaryId();
        else if (event == ButtonEventType::ID_BUTTON_LONG_PRESS_END) finalizeIdSelection();
        else if (event == ButtonEventType::EXEC_BUTTON_PRESS) switchToMode(DeviceMode::MODE_PAIRING); // [NEW] 페어링 진입
        return;
    }

    // [NEW] 페어링 모드 중 취소
    if (_currentMode == DeviceMode::MODE_PAIRING) {
        if (event == ButtonEventType::ID_BUTTON_SHORT_PRESS || event == ButtonEventType::EXEC_BUTTON_PRESS) {
            Log::Info(PSTR("MODE: Pairing Cancelled by User."));
            switchToMode(DeviceMode::MODE_NORMAL);
        }
        return;
    }

    xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
    if (_currentStepIndex != -1 && event != ButtonEventType::NO_EVENT) {
        stopExecutionPlan();
    }
    xSemaphoreGive(_playSequenceMutex);

    switch (event) {
        case ButtonEventType::ID_BUTTON_SHORT_PRESS:
            switchToMode(DeviceMode::MODE_ID_BLINK);
            break;
        case ButtonEventType::ID_BUTTON_LONG_PRESS_END:
            _previousDeviceId = _deviceId;
            switchToMode(DeviceMode::MODE_ID_SET);
            break;
        case ButtonEventType::EXEC_BUTTON_PRESS:
             xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
            if (_currentMode == DeviceMode::MODE_NORMAL && _currentStepIndex == -1 && !_isManualOperationActive) {
                startManualOperation();
            }
             xSemaphoreGive(_playSequenceMutex);
            break;
        case ButtonEventType::EXEC_BUTTON_RELEASE:
             xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
            if (_currentMode == DeviceMode::MODE_NORMAL && _isManualOperationActive) {
                stopManualOperation();
            }
             xSemaphoreGive(_playSequenceMutex);
            break;
        default: break;
    }
}

void ModeManager::triggerManualRun(uint32_t delayMs, uint32_t playMs) {
    if (_currentMode == DeviceMode::MODE_TEST || _currentMode == DeviceMode::MODE_WIFI) {
        Comm::CommPacket manualPkt;
        manualPkt.stepCount = 1;
        manualPkt.steps[0].delayMinutes = (delayMs / 60000);
        manualPkt.steps[0].delaySeconds = (delayMs % 60000) / 1000;
        manualPkt.steps[0].playSeconds = playMs / 1000;
        manualPkt.steps[0].pwmValue = 100;
        
        xSemaphoreTake(_playSequenceMutex, portMAX_DELAY);
        if (_currentStepIndex != -1) stopExecutionPlan();
        startExecutionPlan(manualPkt);
        xSemaphoreGive(_playSequenceMutex);
    }
}

void ModeManager::updateModeNormal() {}
void ModeManager::updateModeIdBlink() {
    if (_hwManager && !_hwManager->isLedPatternActive()) switchToMode(DeviceMode::MODE_NORMAL);
}
void ModeManager::updateModeIdSet() {
    unsigned long currentTime = millis();
    switch (_idSetState) {
        case IdSetState::ENTERED:
            if (currentTime - _idSetLastInputTime > LED_ID_SET_ENTER_ON_MS) {
                _idSetState = IdSetState::AWAITING_INPUT;
                _hwManager->setLedPattern(LedPatternType::LED_OFF);
            }
            break;
        case IdSetState::AWAITING_INPUT:
            if (currentTime - _idSetLastInputTime > ID_SET_TIMEOUT_MS) {
                _deviceId = _previousDeviceId;
                if (_commManager) _commManager->updateMyDeviceId(_deviceId);
                _idSetState = IdSetState::IDLE;
                _hwManager->setLedPattern(LedPatternType::LED_OFF);
                switchToMode(DeviceMode::MODE_NORMAL);
            }
            break;
        case IdSetState::CONFIRMING_ON:
            if (currentTime - _idSetLastInputTime > LED_ID_SET_CONFIRM_ON_MS) {
                _idSetState = IdSetState::CONFIRMING_BLINK;
                _hwManager->setLedPattern(LedPatternType::LED_OFF);
                _idSetLastInputTime = currentTime;
            }
            break;
        case IdSetState::CONFIRMING_BLINK:
            if (!_idBlinkPatternStarted && (currentTime - _idSetLastInputTime >= LED_ID_BLINK_INTERVAL_MS)) {
                _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
                _idBlinkPatternStarted = true;
            }
            if (_idBlinkPatternStarted && !_hwManager->isLedPatternActive()) {
                _idBlinkPatternStarted = false;
                switchToMode(DeviceMode::MODE_NORMAL);
            }
            break;
        default: break;
    }
}
void ModeManager::updateModeWifi() {
    if (millis() - _lastWebApiActivityTime > WIFI_MODE_AUTO_EXIT_MS) exitWifiMode();
}
void ModeManager::updateModeExitWifi() {
    if (_hwManager && !_hwManager->isLedPatternActive()) switchToMode(DeviceMode::MODE_NORMAL);
}

// [NEW] 페어링 모드 타이머(30초 대기)
void ModeManager::updateModePairing() {
    if (millis() - _pairingStartTime > 30000) {
        Log::Warn(PSTR("MODE: Pairing timeout. Returning to NORMAL."));
        switchToMode(DeviceMode::MODE_NORMAL);
    }
}

// [NEW] 통신매니저에서 페어링 패킷을 받았을 때 호출
void ModeManager::notifyPairingSuccess() {
    Log::Info(PSTR("MODE: Pairing Success! Switching to Normal."));
    if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_BOOT_SUCCESS); 
    switchToMode(DeviceMode::MODE_NORMAL);
}

void ModeManager::updateExecutionPlan() {
    if (_currentStepIndex == -1) return;

    unsigned long elapsed = millis() - _playPhaseEndTime; 
    unsigned long accumulatedTime = 0;

    for (int i = 0; i < _planStepCount; i++) {
        const auto& step = _executionPlan[i];
        unsigned long delayMs = (uint32_t)step.delayMinutes * 60000 + (uint32_t)step.delaySeconds * 1000;
        unsigned long playMs = (uint32_t)step.playSeconds * 1000;

        if (elapsed < (accumulatedTime + delayMs)) {
            if (_currentStepIndex != i || !_isDelayPhase) {
                _currentStepIndex = i;
                _isDelayPhase = true;
                if (_hwManager) {
                    _hwManager->setMosfets(0); 
                    _hwManager->setLedPattern(LedPatternType::LED_OFF);
                }
                Log::Info(PSTR("MODE: (S%d/%d) Delay Sync (Elapsed: %lu)."), i + 1, _planStepCount, elapsed);
            }
            return;
        }
        accumulatedTime += delayMs;

        if (elapsed < (accumulatedTime + playMs)) {
            if (_currentStepIndex != i || _isDelayPhase) {
                _currentStepIndex = i;
                _isDelayPhase = false;
                if (_hwManager) {
                    _hwManager->setMosfets(step.pwmValue);
                    _hwManager->setLedPattern(LedPatternType::LED_ON);
                }
                Log::Info(PSTR("MODE: (S%d/%d) Play Sync (Elapsed: %lu, PWM: %d)."), i + 1, _planStepCount, elapsed, step.pwmValue);
            }
            return;
        }
        accumulatedTime += playMs;
    }

    Log::Info(PSTR("MODE: Execution Plan Completed (Total: %lu ms)."), accumulatedTime);
    stopExecutionPlan();
}

void ModeManager::startExecutionPlan(const Comm::CommPacket& pkt) {
    _planStepCount = pkt.stepCount;
    memcpy(_executionPlan, pkt.steps, sizeof(ExecutionStep) * _planStepCount);
    
    _currentCommandId = pkt.txMicros;
    
    _playPhaseEndTime = millis(); 
    _currentStepIndex = -99; 
    
    Log::Info(PSTR("MODE: New Plan Started (%d steps). T0 set to millis()."), _planStepCount);
    updateExecutionPlan(); 
}

void ModeManager::startNextStep() {}

void ModeManager::stopExecutionPlan() {
    if (_currentStepIndex != -1) {
        _currentStepIndex = -1;
        _planStepCount = 0;
        if (_hwManager) {
            _hwManager->setMosfets(0);
            _hwManager->setLedPattern(LedPatternType::LED_OFF);
        }
        _currentCommandId = 0;
        if (_currentMode == DeviceMode::MODE_TEST && _webManager) {
            _webManager->broadcastTestComplete();
        }
    }
}

void ModeManager::incrementTemporaryId() {
    if (_idSetState == IdSetState::ENTERED) _idSetState = IdSetState::AWAITING_INPUT;
    _temporaryId = (_temporaryId == 0) ? MIN_DEVICE_ID : _temporaryId + 1;
    if (_temporaryId > MAX_DEVICE_ID) _temporaryId = MIN_DEVICE_ID;
    _idSetLastInputTime = millis();
    _hwManager->setLedPattern(LedPatternType::LED_ID_SET_INCREMENT);
}

void ModeManager::finalizeIdSelection() {
    if (_idSetState != IdSetState::AWAITING_INPUT) return;
    _idSetState = IdSetState::CONFIRMING_ON;
    _idSetLastInputTime = millis();
    uint8_t finalId = (_temporaryId == 0) ? _previousDeviceId : _temporaryId;
    updateDeviceId(finalId, true);
    _hwManager->setLedPattern(LedPatternType::LED_ID_SET_CONFIRM);
    _idBlinkPatternStarted = false;
}

void ModeManager::updateDeviceId(uint8_t newId, bool saveToNvs) {
    if (newId < MIN_DEVICE_ID || newId > MAX_DEVICE_ID) return;
    _deviceId = newId;
    if (saveToNvs) Utils::saveDeviceId(String(_deviceId));
    if (_commManager) _commManager->updateMyDeviceId(_deviceId);
    if (saveToNvs && _hwManager) _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
}

void ModeManager::recordWebApiActivity() { _lastWebApiActivityTime = millis(); }
void ModeManager::exitWifiMode() { switchToMode(DeviceMode::MODE_EXIT_WIFI); }
const char* ModeManager::getModeName(DeviceMode mode) const {
    switch (mode) {
        case DeviceMode::MODE_BOOT: return "BOOT";
        case DeviceMode::MODE_NORMAL: return "NORMAL";
        case DeviceMode::MODE_ID_BLINK: return "ID_BLINK";
        case DeviceMode::MODE_ID_SET: return "ID_SET";
        case DeviceMode::MODE_WIFI: return "WIFI";
        case DeviceMode::MODE_TEST: return "TEST";
        case DeviceMode::MODE_EXIT_WIFI: return "EXIT_WIFI";
        case DeviceMode::MODE_PAIRING: return "PAIRING"; // [NEW]
        case DeviceMode::MODE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
DeviceMode ModeManager::getCurrentMode() const { return _currentMode; }
const char* ModeManager::getCurrentModeName() const { return getModeName(_currentMode); }
void ModeManager::setUpdateDownloaded(bool downloaded) { _updateDownloaded = downloaded; }

void ModeManager::startManualOperation() {
    _isManualOperationActive = true;
    if (_hwManager) {
        _hwManager->setMosfets(100);
        _hwManager->setLedPattern(LedPatternType::LED_ON);
    }
}

void ModeManager::stopManualOperation() {
    if (!_isManualOperationActive) return;
    _isManualOperationActive = false;
    if (_hwManager) {
        _hwManager->setMosfets(0);
        _hwManager->setLedPattern(LedPatternType::LED_OFF);
    }
}
