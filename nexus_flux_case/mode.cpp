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
    }
}

void ModeManager::startPlaySequence(const ExecutionStep* steps, uint8_t stepCount, uint32_t rttUs, uint32_t rxProcUs) {
    if (stepCount == 0 || stepCount > MAX_EXECUTION_STEPS) return;

    _isPlaySequenceActive = true;
    _planStepCount = stepCount;
    memcpy(_executionPlan, steps, sizeof(ExecutionStep) * stepCount);
    
    long totalCompensationUs = (rttUs / 2) + rxProcUs;
    uint32_t firstDelayMs = (uint32_t)_executionPlan[0].delayMinutes * 60000 + (uint32_t)_executionPlan[0].delaySeconds * 1000;
    long finalAdjustedDelayMs = std::max(0L, (long)firstDelayMs - (totalCompensationUs / 1000L));
    
    _executionPlan[0].delayMinutes = finalAdjustedDelayMs / 60000;
    _executionPlan[0].delaySeconds = (finalAdjustedDelayMs % 60000) / 1000;

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

    if (delayMs > 0) {
        _isDelayPhase = true;
        _hwManager->setMosfets(0);
        _hwManager->setLedPattern(LedPatternType::LED_OFF);
    } else {
        _isDelayPhase = false;
        _phaseEndTime = currentTime + playMs;
        
        uint8_t pwm = currentStep.pwmValue;
        _hwManager->setMosfets(pwm);
        _hwManager->setLedPattern(pwm > 0 ? LedPatternType::LED_ON : LedPatternType::LED_OFF);
    }
}

void ModeManager::stopPlaySequence() {
    if (_isPlaySequenceActive) {
        _isPlaySequenceActive = false;
        _hwManager->setMosfets(0);
        _hwManager->setLedPattern(LedPatternType::LED_OFF);
        
        Log::Info(PSTR("MODE: Sequence %lu completed or stopped."), _currentCommandId);

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

        uint8_t pwm = currentStep.pwmValue;
        _hwManager->setMosfets(pwm);
        _hwManager->setLedPattern(pwm > 0 ? LedPatternType::LED_ON : LedPatternType::LED_OFF);
        
        Log::Info(PSTR("MODE: Step %d Delay -> Play"), _currentStepIndex + 1);
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
        case DeviceMode::MODE_PAIRING:    updateModePairing(); break; 
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
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
        } else if (event == ButtonEventType::EXEC_BUTTON_PRESS) {
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
            switchToMode(DeviceMode::MODE_PAIRING);
        }
        return;
    }

    if (_currentMode == DeviceMode::MODE_PAIRING) {
        if (event == ButtonEventType::ID_BUTTON_SHORT_PRESS || event == ButtonEventType::EXEC_BUTTON_PRESS) {
            Log::Info(PSTR("MODE: Pairing Cancelled by User."));
            switchToMode(DeviceMode::MODE_NORMAL);
        }
        return;
    }

    if (_isPlaySequenceActive && event != ButtonEventType::NO_EVENT) {
        if (millis() - _lastExecButtonActionTime >= 500) {
            Log::Info(PSTR("MODE: Play sequence interrupted by button press."));
            stopPlaySequence();
        }
    }

    switch (event) {
        case ButtonEventType::ID_BUTTON_SHORT_PRESS:
            switchToMode(DeviceMode::MODE_ID_BLINK);
            break;
            
        case ButtonEventType::ID_BUTTON_LONG_PRESS_END:
            _previousDeviceId = _deviceId;
            switchToMode(DeviceMode::MODE_ID_SET);
            break;
            
        case ButtonEventType::EXEC_BUTTON_PRESS:
            if (_currentMode == DeviceMode::MODE_NORMAL) {
                if (_isPlaySequenceActive) {
                    if (millis() - _lastExecButtonActionTime >= 500) {
                         stopPlaySequence();
                    }
                } else {
                    _lastExecButtonActionTime = millis();
                    startManualOperation(); 
                }
            }
            break;
            
        case ButtonEventType::EXEC_BUTTON_RELEASE:
            if (_currentMode == DeviceMode::MODE_NORMAL) {
                stopManualOperation(); 
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
            break;
            
        case DeviceMode::MODE_ID_BLINK:
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
            break;
            
        case DeviceMode::MODE_PAIRING:
            _pairingStartTime = millis();
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_PAIRING); 
            if (_commManager) _commManager->setPairingMode(true); 
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
            
        default:
            break;
    }
}

void ModeManager::attemptAutoConnection() {
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
        WiFi.begin(bestSsid.c_str(), bestPass.c_str());
    }
}

void ModeManager::exitModeLogic(DeviceMode mode) {
    switch (mode) {
        case DeviceMode::MODE_ID_SET:
            _idSetState = IdSetState::IDLE;
            break;
        case DeviceMode::MODE_PAIRING: 
            if (_commManager) _commManager->setPairingMode(false); 
            break;
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
    if (millis() - _lastWebApiActivityTime > WIFI_MODE_AUTO_EXIT_MS) {
        exitWifiMode();
    }
}

void ModeManager::updateModeExitWifi() {
    if (_hwManager && !_hwManager->isLedPatternActive()) {
        switchToMode(DeviceMode::MODE_NORMAL);
    }
}

void ModeManager::notifyPairingSuccess() {
    Log::Info(PSTR("MODE: Pairing Success Notification Received. Switching to Normal."));
    if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_BOOT_SUCCESS); 
    switchToMode(DeviceMode::MODE_NORMAL);
}

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
    
    if (saveToNvs && _hwManager) {
        _hwManager->setLedPattern(LedPatternType::LED_ID_DISPLAY, _deviceId);
    }
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
        case DeviceMode::MODE_PAIRING: return "PAIRING";
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
