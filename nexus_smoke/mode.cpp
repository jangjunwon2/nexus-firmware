#include "mode.h"
#include "hardware.h"
#include "comm.h"
#include "web.h"
#include <algorithm>
#include <esp_now.h>
#include <WiFi.h>
#include "driver/gpio.h"

ModeManager::ModeManager(HardwareManager* hwManager, CommManager* commManager, WebManager* webManager)
    : _hwManager(hwManager), _commManager(commManager), _webManager(webManager),
      _currentMode(DeviceMode::MODE_BOOT),
      _deviceId(DEFAULT_DEVICE_ID),
      _currentCommandId(0),
      _planStepCount(0), 
      _currentStepIndex(0),
      _idSetState(IdSetState::IDLE),
      _temporaryId(0),
      _idSetLastInputTime(0),
      _isPlaySequenceActive(false), 
      _isManualOperationActive(false),
      _isDelayPhase(false), 
      _phaseEndTime(0),
      _lastExecButtonActionTime(0),
      _lastWebApiActivityTime(0), 
      _updateDownloaded(false),
      _idBlinkPatternStarted(false),
      _previousDeviceId(DEFAULT_DEVICE_ID),
      _pairingStartTime(0)
{
    _modeSwitchMutex = xSemaphoreCreateMutex();
    memset(_executionPlan, 0, sizeof(_executionPlan));
}

void ModeManager::begin() {
    if (!_hwManager || !_commManager || !_webManager) {
        Log::Error(PSTR("MODE: Critical manager dependency is missing! Halting."));
        while(1) { vTaskDelay(1000); }
    }

    _deviceId = Utils::loadDeviceId().toInt();
    
    if (_deviceId < MIN_DEVICE_ID || _deviceId > MAX_DEVICE_ID) {
        Log::Warn(PSTR("MODE: Invalid device ID %d loaded from NVS. Resetting to default ID %d."), _deviceId, DEFAULT_DEVICE_ID);
        _deviceId = DEFAULT_DEVICE_ID;
        Utils::saveDeviceId(String(_deviceId));
    }
    
    _commManager->updateMyDeviceId(_deviceId);
    Log::Info(PSTR("MODE: ModeManager initialized. Device ID is %d."), _deviceId);
    
    _hwManager->setLedPattern(LedPatternType::LED_BOOT_SUCCESS);
}

void ModeManager::switchToMode(DeviceMode newMode, bool forceSwitch) {
    if (xSemaphoreTake(_modeSwitchMutex, (TickType_t)0) != pdTRUE) {
        Log::Warn(PSTR("MODE: Switching already in progress. Request to switch to %s ignored."), getModeName(newMode));
        return;
    }

    if (!forceSwitch && _currentMode == newMode) {
        xSemaphoreGive(_modeSwitchMutex);
        return;
    }
    
    bool isStayingInWebUi = 
        (_currentMode == DeviceMode::MODE_WIFI && newMode == DeviceMode::MODE_TEST) ||
        (_currentMode == DeviceMode::MODE_TEST && newMode == DeviceMode::MODE_WIFI);

    if (!isStayingInWebUi) {
        exitModeLogic(_currentMode);
    }
    
    Log::Info(PSTR("MODE: Switching from %s to %s."), getModeName(_currentMode), getModeName(newMode));
    _currentMode = newMode;
    
    if (!isStayingInWebUi) {
        enterModeLogic(_currentMode);
    }

    xSemaphoreGive(_modeSwitchMutex);
}

void ModeManager::handleEspNowCommand(const uint8_t* senderMac, const Comm::CommPacket& pkt, uint32_t rxTime) {
    if (!_commManager || !senderMac) {
        Log::Error(PSTR("MODE: CommManager not initialized or senderMac is null."));
        return;
    }

    // MachineType 필터링 (ALL 이거나 내 타입일 때만)
    if (pkt.targetMachineType != TYPE_ALL && pkt.targetMachineType != MY_MACHINE_TYPE) {
        Log::Debug(PSTR("COMM: Ignored packet for different machine type: %d"), pkt.targetMachineType);
        return; 
    }

    if (_currentMode == DeviceMode::MODE_ID_SET) {
        if (pkt.packetType == Comm::RTT_REQUEST) {
            _commManager->sendAck(senderMac, pkt, rxTime);
        }
        return;
    }

    if (pkt.packetType == Comm::RTT_REQUEST) {
        _commManager->sendAck(senderMac, pkt, rxTime);
    } else if (pkt.packetType == Comm::FINAL_COMMAND) {
        bool isNewCommandSequence = (_currentCommandId != pkt.txButtonPressMicros);
        
        if (isNewCommandSequence) {
            if (_isPlaySequenceActive) {
                stopPlaySequence(); 
            }
            _currentCommandId = pkt.txButtonPressMicros;

            Log::Info(PSTR("MODE: New command (Type: %d) received. Steps: %d"), pkt.targetMachineType, pkt.stepCount);
            
            Utils::saveLastSequence(pkt.steps, pkt.stepCount);
            startPlaySequence(pkt.steps, pkt.stepCount, pkt.lastKnownRttUs, pkt.lastKnownRxProcessingTimeUs);
        }
        _commManager->sendAck(senderMac, pkt, rxTime);
    } else {
        Log::Warn(PSTR("COMM: Unknown packet type %u received. Ignoring."), pkt.packetType);
    }
}

void ModeManager::triggerManualRun(uint32_t delayMs, uint32_t playMs) {
    if (_currentMode == DeviceMode::MODE_TEST || _currentMode == DeviceMode::MODE_WIFI) {
        if (_isPlaySequenceActive) {
            stopPlaySequence();
        }
        _currentCommandId = 0; 
        
        ExecutionStep manualStep;
        manualStep.delayMinutes = delayMs / 60000;
        manualStep.delaySeconds = (delayMs % 60000) / 1000;
        manualStep.playSeconds = playMs / 1000;
        manualStep.pwmValue = 100;

        Utils::saveLastSequence(&manualStep, 1);

        Log::Info(PSTR("MODE: Manual test run started. Delay: %u ms, Play: %u ms."), delayMs, playMs);
        Log::TestLog(PSTR("Manual test: Wait %.1f s, Play %.1f s"), (float)delayMs / 1000.0f, (float)playMs / 1000.0f);
        
        startPlaySequence(&manualStep, 1, 0, 0); 
    } else {
        Log::Warn(PSTR("MODE: Manual run requested in unsupported mode: %s"), getModeName(_currentMode));
    }
}

void ModeManager::startPlaySequence(const ExecutionStep* steps, uint8_t stepCount, uint32_t rttUs, uint32_t rxProcUs) {
    if (stepCount == 0 || stepCount > MAX_EXECUTION_STEPS) {
        Log::Warn(PSTR("MODE: Invalid step count %d received."), stepCount);
        return;
    }

    _isPlaySequenceActive = true;
    _planStepCount = stepCount;
    memcpy(_executionPlan, steps, sizeof(ExecutionStep) * stepCount);
    
    long totalCompensationUs = (rttUs / 2) + rxProcUs;
    uint32_t firstDelayMs = (uint32_t)_executionPlan[0].delayMinutes * 60000 + (uint32_t)_executionPlan[0].delaySeconds * 1000;
    long finalAdjustedDelayMs = std::max(0L, (long)firstDelayMs - (totalCompensationUs / 1000L));
    
    _executionPlan[0].delayMinutes = finalAdjustedDelayMs / 60000;
    _executionPlan[0].delaySeconds = (finalAdjustedDelayMs % 60000) / 1000;

    Log::Info(PSTR("MODE: Play sequence started. Total steps: %d. First delay compensated: %ld ms"), _planStepCount, finalAdjustedDelayMs);
    
    _currentStepIndex = 0;
    startNextStep();
}

void ModeManager::startNextStep() {
    if (_currentStepIndex >= _planStepCount) {
        stopPlaySequence();
        return;
    }

    const ExecutionStep& currentStep = _executionPlan[_currentStepIndex];
    uint32_t delayMs = (uint32_t)currentStep.delayMinutes * 60000 + (uint32_t)currentStep.delaySeconds * 1000;
    uint32_t playMs = (uint32_t)currentStep.playSeconds * 1000;
    
    unsigned long currentTime = millis();
    _phaseEndTime = currentTime + delayMs;

    Log::Info(PSTR("MODE: Starting Step %d/%d (Delay: %lu ms, Play: %lu ms)"), _currentStepIndex + 1, _planStepCount, delayMs, playMs);
    Log::TestLog(PSTR("Step %d: Wait %.1fs, Play %.1fs"), _currentStepIndex + 1, (float)delayMs / 1000.0f, (float)playMs / 1000.0f);

    if (delayMs > 0) {
        _isDelayPhase = true;
        _hwManager->setMosfets(false);
        _hwManager->setLedPattern(LedPatternType::LED_OFF);
    } else {
        _isDelayPhase = false;
        _phaseEndTime = currentTime + playMs;
        
        bool turnOn = (currentStep.pwmValue > 0);
        _hwManager->setMosfets(turnOn);
        _hwManager->setLedPattern(turnOn ? LedPatternType::LED_ON : LedPatternType::LED_OFF);
    }
}

void ModeManager::stopPlaySequence() {
    if (_isPlaySequenceActive) {
        _isPlaySequenceActive = false;
        _hwManager->setMosfets(false);
        _hwManager->setLedPattern(LedPatternType::LED_OFF);
        
        Log::Info(PSTR("MODE: Sequence %lu completed or stopped."), _currentCommandId);
        if (_currentCommandId != 0) {
            Log::TestLog(PSTR("Sequence finished."));
        }

        _currentCommandId = 0;
        _planStepCount = 0;
        _currentStepIndex = 0;

        if (_currentMode == DeviceMode::MODE_TEST) {
             if (_webManager) _webManager->broadcastTestComplete();
        }
    }
}

void ModeManager::updatePlaySequence() {
    if (!_isPlaySequenceActive) return;

    unsigned long currentTime = millis();
    if (currentTime < _phaseEndTime) return;

    if (_isDelayPhase) {
        _isDelayPhase = false;
        const ExecutionStep& currentStep = _executionPlan[_currentStepIndex];
        uint32_t playMs = (uint32_t)currentStep.playSeconds * 1000;
        _phaseEndTime = currentTime + playMs;

        bool turnOn = (currentStep.pwmValue > 0);
        _hwManager->setMosfets(turnOn);
        _hwManager->setLedPattern(turnOn ? LedPatternType::LED_ON : LedPatternType::LED_OFF);
        
        Log::Info(PSTR("MODE: Step %d Delay -> Play"), _currentStepIndex + 1);
        Log::TestLog(PSTR("Step %d: Delay finished, now playing."), _currentStepIndex + 1);
    } else {
        Log::Info(PSTR("MODE: Step %d Play finished."), _currentStepIndex + 1);
        _currentStepIndex++;
        if (_currentStepIndex < _planStepCount) {
            startNextStep();
        } else {
            stopPlaySequence();
        }
    }
}

void ModeManager::update() {
    if(_hwManager) {
        handleButtonEvent(_hwManager->getButtonEvent());
    }
    
    if (_isPlaySequenceActive) {
        updatePlaySequence();
    }

    switch (_currentMode) {
        case DeviceMode::MODE_NORMAL:     updateModeNormal();  break;
        case DeviceMode::MODE_ID_BLINK:   updateModeIdBlink(); break;
        case DeviceMode::MODE_ID_SET:     updateModeIdSet();   break;
        case DeviceMode::MODE_WIFI:
        case DeviceMode::MODE_TEST:       updateModeWifi();    break;
        case DeviceMode::MODE_EXIT_WIFI:  updateModeExitWifi(); break;
        case DeviceMode::MODE_PAIRING:    updateModePairing(); break; // 페어링 타임아웃 처리
        default: break;
    }
}

void ModeManager::handleButtonEvent(ButtonEventType event) {
    if (event == ButtonEventType::NO_EVENT) return;

    if (event == ButtonEventType::BOTH_BUTTONS_LONG_PRESS) {
        stopManualOperation();
        stopPlaySequence();

        if (_currentMode == DeviceMode::MODE_WIFI || _currentMode == DeviceMode::MODE_TEST) {
            switchToMode(DeviceMode::MODE_EXIT_WIFI);
        } else {
            switchToMode(DeviceMode::MODE_WIFI);
        }
        return;
    }

    if (_currentMode == DeviceMode::MODE_WIFI || _currentMode == DeviceMode::MODE_TEST) {
        if (event == ButtonEventType::ID_BUTTON_SHORT_PRESS) {
            Log::Info(PSTR("MODE: Displaying ID in Wi-Fi/Test mode."));
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
            Log::TestLog(PSTR("ID button short pressed. Showing ID %d."), _deviceId);
        } else if (event == ButtonEventType::ID_BUTTON_LONG_PRESS_END) {
            Log::Warn(PSTR("MODE: ID setting is disabled in Wi-Fi/Test mode."));
            Log::TestLog(PSTR("ID button long pressed. ID setting disabled in Test mode."));
        }

        if (event == ButtonEventType::EXEC_BUTTON_PRESS) {
            if (!_isPlaySequenceActive && !_isManualOperationActive) {
                startManualOperation();
            }
        } else if (event == ButtonEventType::EXEC_BUTTON_RELEASE) {
            if (_isManualOperationActive) {
                stopManualOperation();
            }
        }
        return;
    }

    if (_currentMode == DeviceMode::MODE_ID_SET) {
        _idSetLastInputTime = millis();
        if (event == ButtonEventType::ID_BUTTON_SHORT_PRESS) {
            incrementTemporaryId();
        } else if (event == ButtonEventType::ID_BUTTON_LONG_PRESS_END) {
            finalizeIdSelection();
        } else if (event == ButtonEventType::EXEC_BUTTON_PRESS) {
            // [NEW] ID 설정 모드 대기 상태에서 EXEC 버튼 누르면 페어링 진입
            switchToMode(DeviceMode::MODE_PAIRING);
        }
        return;
    }

    // [NEW] 페어링 모드 중에 아무 버튼이나 누르면 즉시 취소
    if (_currentMode == DeviceMode::MODE_PAIRING) {
        if (event == ButtonEventType::ID_BUTTON_SHORT_PRESS || event == ButtonEventType::EXEC_BUTTON_PRESS) {
            Log::Info(PSTR("MODE: Pairing Cancelled by User."));
            switchToMode(DeviceMode::MODE_NORMAL);
        }
        return;
    }

    if (_isPlaySequenceActive && event != ButtonEventType::NO_EVENT) {
        if (millis() - _lastExecButtonActionTime < 500) {
            Log::Debug(PSTR("MODE: Button press ignored (debounce/cooldown)."));
        } else {
            Log::Info(PSTR("MODE: Play sequence interrupted by button press."));
            stopPlaySequence();
        }
    }

    switch (event) {
        case ButtonEventType::ID_BUTTON_SHORT_PRESS:
            switchToMode(DeviceMode::MODE_ID_BLINK);
            Log::TestLog(PSTR("ID button short pressed. Showing device ID."));
            break;
            
        case ButtonEventType::ID_BUTTON_LONG_PRESS_END:
            _previousDeviceId = _deviceId;
            switchToMode(DeviceMode::MODE_ID_SET);
            Log::TestLog(PSTR("ID button long pressed. Entering ID setting mode."));
            break;
            
        case ButtonEventType::EXEC_BUTTON_PRESS:
            // [수정됨] EXEC 버튼 누르면 수동 동작 시작 (시퀀스 재생 안 함)
            if (_currentMode == DeviceMode::MODE_NORMAL) {
                if (_isPlaySequenceActive) {
                    if (millis() - _lastExecButtonActionTime >= 500) {
                         Log::Info(PSTR("MODE: Button pressed. Stopping current sequence."));
                         stopPlaySequence();
                    }
                } else {
                    _lastExecButtonActionTime = millis();
                    startManualOperation(); // 버튼을 누를 때 켜기
                }
            }
            break;
            
        case ButtonEventType::EXEC_BUTTON_RELEASE:
            // [수정됨] EXEC 버튼 떼면 수동 동작 정지
            if (_currentMode == DeviceMode::MODE_NORMAL) {
                stopManualOperation(); // 버튼에서 손을 떼면 끄기
            }
            break;
            
        default: break;
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
            Log::Info(PSTR("MODE: Entered ID setting mode. Current ID: %d"), _deviceId);
            break;
            
        case DeviceMode::MODE_ID_BLINK:
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
            Log::Info(PSTR("MODE: Entered ID blink mode. Displaying ID: %d"), _deviceId);
            break;
            
        // [NEW] 페어링 모드 시작 시
        case DeviceMode::MODE_PAIRING:
            _pairingStartTime = millis();
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_PAIRING); // 빠른 점멸
            if (_commManager) _commManager->setPairingMode(true); // 통신매니저 가로채기 허가
            Log::Info(PSTR("MODE: Entered Pairing Mode. Press 'SYNC(MAIN)' on Transmitter..."));
            break;

        case DeviceMode::MODE_WIFI:
            stopPlaySequence();
            if (_hwManager) _hwManager->shutdownOutputs();
            _hwManager->setLedPattern(LedPatternType::LED_WIFI_MODE_TOGGLE);
            _lastWebApiActivityTime = millis();
            attemptAutoConnection(); 
            if (_webManager) _webManager->startServer();
            break;
            
        case DeviceMode::MODE_TEST:
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ON);
            Log::Info(PSTR("MODE: Entered test mode."));
            break;
            
        case DeviceMode::MODE_EXIT_WIFI:
            if (_webManager && _webManager->isServerRunning() && _updateDownloaded) {
                // 재부팅 전 정리 — 생략 시 새 펌웨어 첫 부팅에서 먹통 발생
                if (_webManager) _webManager->stopServer();
                WiFi.softAPdisconnect(true);
                vTaskDelay(pdMS_TO_TICKS(500));
                if (_hwManager) _hwManager->shutdownOutputs();
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
            Log::Error(PSTR("MODE: Entered ERROR mode. System halted."));
            break;
            
        default:
            break;
    }
}

void ModeManager::attemptAutoConnection() {
    Log::Info(PSTR("MODE: Attempting Wi-Fi auto-connection..."));
    String credsJson = Utils::loadWifiCredentials();
    if (credsJson == "[]") {
        Log::Info(PSTR("MODE: No saved Wi-Fi credentials. Skipping auto-connection."));
        return;
    }
    
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    
    if (n == 0) {
        Log::Warn(PSTR("MODE: No networks found during auto-connect scan."));
        return;
    }
    
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
        Log::Info(PSTR("MODE: Found best-match saved network: %s (RSSI: %d). Connecting..."), bestSsid.c_str(), maxRssi);
        WiFi.begin(bestSsid.c_str(), bestPass.c_str());
    } else {
        Log::Info(PSTR("MODE: No saved networks found in scan results."));
    }
}

void ModeManager::exitModeLogic(DeviceMode mode) {
    switch (mode) {
        case DeviceMode::MODE_ID_SET:
            _idSetState = IdSetState::IDLE;
            break;
            
        // [NEW] 페어링 모드 해제 시 통신매니저의 플래그 끄기
        case DeviceMode::MODE_PAIRING: 
            if (_commManager) _commManager->setPairingMode(false); 
            break;
            
        case DeviceMode::MODE_WIFI:
        case DeviceMode::MODE_TEST:
        default: break;
    }
}

void ModeManager::applyUpdateAndReboot() {
    if (_webManager) _webManager->performUpdateAndReboot();
}

void ModeManager::updateModeNormal() {}

void ModeManager::updateModeIdBlink() {
    if (_hwManager && !_hwManager->isLedPatternActive()) {
        switchToMode(DeviceMode::MODE_NORMAL);
    }
}

void ModeManager::updateModeIdSet() {
    unsigned long currentTime = millis();
    switch (_idSetState) {
        case IdSetState::ENTERED:
            if (currentTime - _idSetLastInputTime > LED_ID_SET_ENTER_ON_MS) {
                _idSetState = IdSetState::AWAITING_INPUT;
                _hwManager->setLedPattern(LedPatternType::LED_OFF);
                Log::Info(PSTR("MODE: Ready to receive ID input."));
            }
            break;
        case IdSetState::AWAITING_INPUT:
            if (currentTime - _idSetLastInputTime > ID_SET_TIMEOUT_MS) {
                Log::Info(PSTR("MODE: ID setting mode timed out. Reverting to previous ID: %d"), _previousDeviceId);
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
                Log::Info(PSTR("MODE: 1 second solid ON complete, waiting 200ms then starting ID %d blink."), _deviceId);
            }
            break;
        case IdSetState::CONFIRMING_BLINK:
            if (!_idBlinkPatternStarted && (currentTime - _idSetLastInputTime >= LED_ID_BLINK_INTERVAL_MS)) {
                _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
                _idBlinkPatternStarted = true;
                Log::Debug(PSTR("MODE: ID blinking started (ID: %d)."), _deviceId);
            }
            if (_idBlinkPatternStarted && !_hwManager->isLedPatternActive()) {
                Log::Info(PSTR("MODE: ID blinking complete. Switching to Normal mode."));
                _idBlinkPatternStarted = false;
                switchToMode(DeviceMode::MODE_NORMAL);
            }
            break;
        default: break;
    }
}

void ModeManager::updateModeWifi() {
    if (millis() - _lastWebApiActivityTime > WIFI_MODE_AUTO_EXIT_MS) {
        Log::Info(PSTR("MODE: Wi-Fi mode inactive for %d minutes. Exiting."), WIFI_MODE_AUTO_EXIT_MS / 60000);
        exitWifiMode();
    }
}

void ModeManager::updateModeExitWifi() {
    if (_hwManager && !_hwManager->isLedPatternActive()) {
        switchToMode(DeviceMode::MODE_NORMAL);
    }
}

// [NEW] 페어링 성공 시 통신매니저에서 호출
void ModeManager::notifyPairingSuccess() {
    Log::Info(PSTR("MODE: Pairing Success Notification Received. Switching to Normal."));
    if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_BOOT_SUCCESS); // 성공의 의미로 1초 켜짐
    switchToMode(DeviceMode::MODE_NORMAL);
}

// [NEW] 페어링 30초 대기열
void ModeManager::updateModePairing() {
    if (millis() - _pairingStartTime > 30000) {
        Log::Warn(PSTR("MODE: Pairing timeout. Returning to NORMAL."));
        switchToMode(DeviceMode::MODE_NORMAL);
    }
}

void ModeManager::incrementTemporaryId() {
    if (_idSetState == IdSetState::ENTERED) _idSetState = IdSetState::AWAITING_INPUT;
    
    _temporaryId = (_temporaryId == 0) ? MIN_DEVICE_ID : _temporaryId + 1;
    if (_temporaryId > MAX_DEVICE_ID) _temporaryId = MIN_DEVICE_ID;
    
    _idSetLastInputTime = millis();
    _hwManager->setLedPattern(LedPatternType::LED_ID_SET_INCREMENT);
    
    Log::Info(PSTR("MODE: Temporary ID set to %d."), _temporaryId);
    Log::TestLog(PSTR("Temp ID: %d."), _temporaryId);
}

void ModeManager::finalizeIdSelection() {
    if (_idSetState != IdSetState::AWAITING_INPUT) return;
    
    _idSetState = IdSetState::CONFIRMING_ON;
    _idSetLastInputTime = millis();

    uint8_t finalId;
    if (_temporaryId == 0) {
        finalId = _previousDeviceId;
        Log::Info(PSTR("MODE: Temporary ID is 0, finalizing with previous ID %d."), finalId);
    } else {
        finalId = _temporaryId;
    }

    updateDeviceId(finalId, true);

    Log::Info(PSTR("MODE: ID setting confirmed - ID: %d, starting 1 sec solid ON."), finalId);
    _hwManager->setLedPattern(LedPatternType::LED_ID_SET_CONFIRM);
    _idBlinkPatternStarted = false;
    Log::TestLog(PSTR("ID confirmed: %d."), finalId);
}

void ModeManager::updateDeviceId(uint8_t newId, bool saveToNvs) {
    if (newId < MIN_DEVICE_ID || newId > MAX_DEVICE_ID) {
        Log::Warn(PSTR("MODE: Attempted to set invalid Device ID: %d. Keeping current ID: %d."), newId, _deviceId);
        return;
    }
    
    _deviceId = newId;
    if (saveToNvs) {
        Utils::saveDeviceId(String(_deviceId));
    }
    
    if (_commManager) {
        _commManager->updateMyDeviceId(_deviceId);
    }
    
    Log::Info(PSTR("MODE: Device ID set to %d."), _deviceId);
    
    if (saveToNvs && _hwManager) {
        _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
        Log::TestLog(PSTR("ID changed from web: %d"), _deviceId);
    }
}

void ModeManager::recordWebApiActivity() { 
    _lastWebApiActivityTime = millis();
}

void ModeManager::exitWifiMode() { 
    switchToMode(DeviceMode::MODE_EXIT_WIFI);
}

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

DeviceMode ModeManager::getCurrentMode() const { 
    return _currentMode; 
}

const char* ModeManager::getCurrentModeName() const { 
    return getModeName(_currentMode); 
}

void ModeManager::setUpdateDownloaded(bool downloaded) { 
    _updateDownloaded = downloaded; 
}

void ModeManager::startManualOperation() {
    _isManualOperationActive = true;
    if (_hwManager) {
        _hwManager->setMosfets(true);
        _hwManager->setLedPattern(LedPatternType::LED_ON);
    }
    Log::TestLog(PSTR("EXEC button pressed. Device operating."));
}

void ModeManager::stopManualOperation() {
    if (!_isManualOperationActive) return;
    _isManualOperationActive = false;
    if (_hwManager) {
        _hwManager->setMosfets(false);
        _hwManager->setLedPattern(LedPatternType::LED_OFF);
    }
    Log::Info(PSTR("MODE: Manual execution released after %lu ms."), _hwManager->getExecButtonPressedDuration());
    Log::TestLog(PSTR("EXEC button released. Device stopped."));
}
