#include "mode.h"
#include "hardware.h"
#include "comm.h"
#include "web.h"
#include <algorithm>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "driver/gpio.h"

ModeManager::ModeManager(HardwareManager* hwManager, CommManager* commManager, WebManager* webManager)
    : _hwManager(hwManager), _commManager(commManager), _webManager(webManager),
      _currentMode(DeviceMode::MODE_BOOT),
      _deviceId(DEFAULT_DEVICE_ID),
      _currentCommandId(0),
      _lastAckedTxMicros(0),
      _ackMux(portMUX_INITIALIZER_UNLOCKED),
      _ackPending(false),
      _ackOrigTxMicros(0),
      _ackQueuedAt(0),
      _planStepCount(0),
      _currentStepIndex(0),
      _idSetState(IdSetState::IDLE),
      _temporaryId(0),
      _idSetLastInputTime(0),
      _isPlaySequenceActive(false),
      _isManualOperationActive(false),
      _isDelayPhase(false),
      _phaseStartMs(0),
      _phaseDuration(0),
      _lastExecButtonActionTime(0),
      _lastWebApiActivityTime(0), 
      _updateDownloaded(false),
      _idBlinkPatternStarted(false),
      _previousDeviceId(DEFAULT_DEVICE_ID),
      _pairingStartTime(0),
      _timedRunDurationMs(DEFAULT_TIMED_RUN_MS),
      _lastRemoteTestPlayMs(0),
      _firstStepOverrideDelayMs(UINT32_MAX),
      _scanStepStartTime(0),
      _scanStepIdx(0),
      _lastScanReportTime(0),
      _scanReportAttempts(0),
      _pendingScanStart(false),
      _pendingChannelCommit(false),
      _committedChannel(1),
      _holdMux(portMUX_INITIALIZER_UNLOCKED),
      _holdReq(false),
      _holdPwm(0),
      _lastHoldMs(0),
      _holdOn(false),
      _holdBlocked(false),
      _holdStartMs(0),
      _rfScanBestCh(1),
      _manualRunMux(portMUX_INITIALIZER_UNLOCKED),
      _pendingManualRun(false),
      _pendingManualDelayMs(0),
      _pendingManualPlayMs(0),
      _pendingTimedRunSave(false),
      _pendingTimedRunMs(0)
{
    _modeSwitchMutex = xSemaphoreCreateMutex();
    memset(_executionPlan, 0, sizeof(_executionPlan));
    memset(_scanSuccessRates, 0, sizeof(_scanSuccessRates));
}

void ModeManager::begin() {
    if (!_hwManager || !_commManager || !_webManager) {
        Log::Error(PSTR("MODE: Critical manager dependency is missing! Halting."));
        while(1) { vTaskDelay(1000); }
    }

    _deviceId = Utils::loadDeviceId().toInt();
    _timedRunDurationMs = Utils::loadTimedRunMs();
    Log::Info(PSTR("MODE: Timed run duration loaded: %lu ms."), _timedRunDurationMs);

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
    if (xSemaphoreTake(_modeSwitchMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
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
        return;
    }

    if (pkt.packetType == Comm::FIRE_COMMAND) {
        // [지연 ACK] 송신기 FIRE 버스트(반이중) 중엔 수신 불가 → ACK를 ~12ms 뒤 메인루프에서 송출.
        // 같은 txMicros(버스트/재시도 사본)는 1회만 큐잉(중복 ACK 방지).
        taskENTER_CRITICAL(&_ackMux);
        bool newTx = (pkt.txMicros != _lastAckedTxMicros);
        if (newTx) {
            _lastAckedTxMicros = pkt.txMicros;
            _ackOrigTxMicros   = pkt.txMicros;
            _ackQueuedAt       = millis();
            _ackPending        = true;
        }
        taskEXIT_CRITICAL(&_ackMux);

        bool isNewCommandSequence = (_currentCommandId != pkt.txButtonPressMicros);
        if (isNewCommandSequence) {
            if (_isPlaySequenceActive) stopPlaySequence();
            _currentCommandId = pkt.txButtonPressMicros;

            Log::Info(PSTR("MODE: FIRE received (Type: %d, Steps: %d)"), pkt.targetMachineType, pkt.stepCount);

            // [FIX] saveLastSequence(NVS flash) 제거 — flash 쓰기가 양쪽 코어를 멈춰 통신을 깨뜨리고,
            //       loadLastSequence는 어디서도 호출 안 되는 죽은 기능이라 저장 불필요.

            // 리모컨 명령의 총 play 시간을 타이머 기준 시간으로 저장 (device 고유 기능)
            uint32_t totalPlayMs = 0;
            for (uint8_t i = 0; i < pkt.stepCount; i++) {
                totalPlayMs += (uint32_t)pkt.steps[i].playSeconds * 1000;
            }
            if (totalPlayMs > 0) {
                _timedRunDurationMs = totalPlayMs;
                _lastRemoteTestPlayMs = totalPlayMs;
                _pendingTimedRunMs = totalPlayMs;
                _pendingTimedRunSave = true;
                Log::Info(PSTR("MODE: Timed run duration updated to %lu ms via remote."), totalPlayMs);
            }

            uint32_t elapsedSinceButtonUs = pkt.txMicros - pkt.txButtonPressMicros;
            startPlaySequence(pkt.steps, pkt.stepCount, elapsedSinceButtonUs);
        }
        // 중복(버스트/재시도)은 조용히 무시 — ACK는 위에서 이미 큐잉됨
    }
}

void ModeManager::triggerManualRun(uint32_t delayMs, uint32_t playMs) {
    // HTTP 핸들러(Core 0)에서 호출 → startPlaySequence/NVS 직접 호출 금지.
    // 스핀락으로 보호된 플래그만 세우고 실제 실행은 update()(Core 1)에서.
    taskENTER_CRITICAL(&_manualRunMux);
    _pendingManualDelayMs = delayMs;
    _pendingManualPlayMs  = playMs;
    _pendingManualRun     = true;
    taskEXIT_CRITICAL(&_manualRunMux);
}

void ModeManager::startPlaySequence(const ExecutionStep* steps, uint8_t stepCount, uint32_t elapsedSinceButtonUs) {
    if (stepCount == 0 || stepCount > MAX_EXECUTION_STEPS) return;

    _isPlaySequenceActive = true;
    _planStepCount = stepCount;
    memcpy(_executionPlan, steps, sizeof(ExecutionStep) * stepCount);

    uint64_t firstDelayUs64 = (uint64_t)_executionPlan[0].delayMinutes * 60000000ULL
                           + (uint64_t)_executionPlan[0].delaySeconds * 1000000ULL;
    uint32_t firstDelayUs = (firstDelayUs64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)firstDelayUs64;
    uint32_t adjustedDelayUs = (firstDelayUs > elapsedSinceButtonUs)
                               ? firstDelayUs - elapsedSinceButtonUs : 0;
    uint32_t adjustedDelayMs = adjustedDelayUs / 1000;

    _firstStepOverrideDelayMs = adjustedDelayMs;

    _currentStepIndex = 0;
    startNextStep();
}

void ModeManager::startNextStep() {
    if (_currentStepIndex >= _planStepCount) {
        stopPlaySequence();
        return;
    }

    const ExecutionStep& currentStep = _executionPlan[_currentStepIndex];
    uint32_t delayMs;
    if (_currentStepIndex == 0 && _firstStepOverrideDelayMs != UINT32_MAX) {
        delayMs = _firstStepOverrideDelayMs;
        _firstStepOverrideDelayMs = UINT32_MAX;
    } else {
        delayMs = (uint32_t)currentStep.delayMinutes * 60000 + (uint32_t)currentStep.delaySeconds * 1000;
    }
    uint32_t playMs = (uint32_t)currentStep.playSeconds * 1000;
    
    unsigned long currentTime = millis();

    Log::Info(PSTR("MODE: Starting Step %d/%d (Delay: %lu ms, Play: %lu ms)"), _currentStepIndex + 1, _planStepCount, delayMs, playMs);
    Log::TestLog(PSTR("Step %d: Wait %.1fs, Play %.1fs"), _currentStepIndex + 1, (float)delayMs / 1000.0f, (float)playMs / 1000.0f);

    if (delayMs > 0) {
        _isDelayPhase = true;
        _phaseStartMs = currentTime;
        _phaseDuration = delayMs;
        _hwManager->setMosfets(false);
        _hwManager->setLedPattern(LedPatternType::LED_OFF);
    } else {
        _isDelayPhase = false;
        _phaseStartMs = currentTime;
        _phaseDuration = playMs;
        bool turnOn = (currentStep.pwmValue > 0);
        _hwManager->setMosfets(currentStep.pwmValue);
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
    if ((unsigned long)(currentTime - _phaseStartMs) < _phaseDuration) return;

    if (_isDelayPhase) {
        _isDelayPhase = false;
        const ExecutionStep& currentStep = _executionPlan[_currentStepIndex];
        uint32_t playMs = (uint32_t)currentStep.playSeconds * 1000;
        _phaseStartMs = currentTime;
        _phaseDuration = playMs;

        bool turnOn = (currentStep.pwmValue > 0);
        _hwManager->setMosfets(currentStep.pwmValue);
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
    serviceHoldOutput();

    // [지연 ACK] 송신기 FIRE 버스트가 끝났을 시점(~12ms 후)에 ACK 송출 → 반이중 충돌 회피
    taskENTER_CRITICAL(&_ackMux);
    bool ackReady = _ackPending && ((millis() - _ackQueuedAt) >= 12);
    uint32_t ackTx = _ackOrigTxMicros;
    if (ackReady) _ackPending = false;
    taskEXIT_CRITICAL(&_ackMux);
    if (ackReady && _commManager) _commManager->sendAckBroadcast(ackTx);

    // Core 0 HTTP 핸들러에서 요청된 수동 실행 처리
    taskENTER_CRITICAL(&_manualRunMux);
    bool manualPending   = _pendingManualRun;
    uint32_t manualDelay = _pendingManualDelayMs;
    uint32_t manualPlay  = _pendingManualPlayMs;
    if (manualPending) _pendingManualRun = false;
    taskEXIT_CRITICAL(&_manualRunMux);

    if (manualPending && (_currentMode == DeviceMode::MODE_TEST || _currentMode == DeviceMode::MODE_WIFI)) {
        if (_isPlaySequenceActive) stopPlaySequence();
        _currentCommandId = 0;
        ExecutionStep manualStep;
        uint32_t delayM = manualDelay / 60000; if (delayM > 255) delayM = 255;
        manualStep.delayMinutes = (uint8_t)delayM;
        manualStep.delaySeconds = (manualDelay % 60000) / 1000;
        uint32_t playS = manualPlay / 1000; if (playS > 255) playS = 255;
        manualStep.playSeconds  = (uint8_t)playS;
        manualStep.pwmValue     = 100;
        if (manualPlay > 0) {
            _timedRunDurationMs   = manualPlay;
            _lastRemoteTestPlayMs = manualPlay;
            _pendingTimedRunMs    = manualPlay;
            _pendingTimedRunSave  = true;
        }
        Log::Info(PSTR("MODE: Manual test run started. Delay: %u ms, Play: %u ms."), manualDelay, manualPlay);
        Log::TestLog(PSTR("Manual test: Wait %.1f s, Play %.1f s"), (float)manualDelay / 1000.0f, (float)manualPlay / 1000.0f);
        startPlaySequence(&manualStep, 1, 0);
    }

    // NVS 쓰기 지연 처리 (Core 0 콜백/HTTP에서 요청된 것)
    if (_pendingTimedRunSave) {
        _pendingTimedRunSave = false;
        Utils::saveTimedRunMs(_pendingTimedRunMs);
        Log::Info(PSTR("MODE: Timed run duration saved: %lu ms"), _pendingTimedRunMs);
    }

    // ESP-NOW 콜백 컨텍스트에서 esp_wifi_set_channel 호출 금지 → 메인루프에서 처리
    if (_pendingScanStart) {
        _pendingScanStart = false;
        if (_currentMode != DeviceMode::MODE_RF_SCAN) {
            switchToMode(DeviceMode::MODE_RF_SCAN, true);
        }
    }

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
        case DeviceMode::MODE_RF_SCAN:    updateModeRfScan();  break;
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
            // 플럭스 케이스: 1회 누르면 _timedRunDurationMs 동안 동작 후 자동 정지 (hold 방식 아님)
            if (_currentMode == DeviceMode::MODE_NORMAL) {
                if (_isPlaySequenceActive) {
                    if (millis() - _lastExecButtonActionTime >= 500) {
                        Log::Info(PSTR("MODE: Button pressed. Stopping current sequence."));
                        stopPlaySequence();
                    }
                } else {
                    _lastExecButtonActionTime = millis();
                    ExecutionStep timedStep;
                    memset(&timedStep, 0, sizeof(timedStep));
                    uint32_t playS = _timedRunDurationMs / 1000;
                    if (playS == 0) playS = 1;
                    if (playS > 255) playS = 255;
                    timedStep.playSeconds = (uint8_t)playS;
                    timedStep.pwmValue = 100;
                    Log::Info(PSTR("MODE: Timed run started: %lu ms (%u s)."), _timedRunDurationMs, timedStep.playSeconds);
                    Log::TestLog(PSTR("EXEC: Timed run %u s."), timedStep.playSeconds);
                    startPlaySequence(&timedStep, 1, 0);
                }
            }
            break;

        case ButtonEventType::EXEC_BUTTON_RELEASE:
            // 플럭스 케이스: 버튼 릴리즈 시 동작 중단 없음 (타이머가 자동 정지)
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
            {
                _pairingStartTime = millis();
                if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_PAIRING); // 빠른 점멸
                esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); // [FIX] 페어링 채널 1번 고정
                if (_commManager) _commManager->setPairingMode(true); // 통신매니저 가로채기 허가
                
                uint8_t primaryChan = 0;
                wifi_second_chan_t secondChan = WIFI_SECOND_CHAN_NONE;
                esp_wifi_get_channel(&primaryChan, &secondChan);
                Log::Info(PSTR("MODE: Entered Pairing Mode. Press 'PAIRING' on Transmitter... (HW Ch: %d)"), primaryChan);
            }
            break;

        case DeviceMode::MODE_WIFI:
            stopPlaySequence();
            if (_hwManager) _hwManager->shutdownOutputs();
            _hwManager->setLedPattern(LedPatternType::LED_WIFI_MODE_TOGGLE);
            _lastWebApiActivityTime = millis();
            if (_commManager) _commManager->prepareForWifiMode();
            if (_webManager) _webManager->startServer();
            attemptAutoConnection();
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
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                vTaskDelay(pdMS_TO_TICKS(300));
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
            
        case DeviceMode::MODE_RF_SCAN:
            _scanStepIdx = 0;
            _scanStepStartTime = millis();
            memset(_scanSuccessRates, 0, sizeof(_scanSuccessRates));
            _scanReportAttempts = 0;
            _lastScanReportTime = 0;
            _pendingChannelCommit = false; // 이전 스캔의 잔여 확정 신호 무효화
            esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_PAIRING);
            Log::Info(PSTR("MODE: RF Scan Started. Current Ch 1..."));
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
    
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    
    if (n <= 0) {
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
            
        case DeviceMode::MODE_RF_SCAN:
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_OFF);
            // 반복적인 채널 변경으로 손상된 ESP-NOW 수신 상태 복원
            if (_commManager) _commManager->reinitForEspNow();
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

// [NEW] RF 자동 스캔 수신 및 측정 관리 구현
void ModeManager::handleScanStartCommand() {
    // ESP-NOW 수신 콜백 컨텍스트 → esp_wifi_set_channel 직접 호출 금지.
    // _pendingScanStart 플래그만 세우고 실제 전환은 메인루프 update()에서 처리.
    if (_currentMode != DeviceMode::MODE_RF_SCAN) {
        Log::Info(PSTR("MODE: SCAN_START received. Deferring AUTO CH switch to main loop."));
        _pendingScanStart = true;
    }
}

// 송신기의 채널 확정 신호 수신 (ESP-NOW 콜백) → 플래그만 세팅, 실제 전환은 메인루프에서.
void ModeManager::handleChannelCommit(uint8_t channel) {
    if (channel != 1 && channel != 6 && channel != 11) {
        Log::Warn(PSTR("COMM: Invalid channel commit %d — ignored"), channel);
        return;
    }
    _committedChannel = channel;
    _pendingChannelCommit = true;
}

void ModeManager::handleHold(uint8_t pwm, uint8_t active) {
    taskENTER_CRITICAL(&_holdMux);
    _lastHoldMs = millis();
    if (active == 1) {
        if (!_holdBlocked) { _holdPwm = pwm; _holdReq = true; }
    } else if (active == 0) {
        _holdReq = false;
        _holdBlocked = false;
    }
    taskEXIT_CRITICAL(&_holdMux);
}

void ModeManager::cancelPlaySequence() {
    Log::Info(PSTR("MODE: Play sequence cancelled by transmitter."));
    if (_isPlaySequenceActive) stopPlaySequence();
    taskENTER_CRITICAL(&_holdMux);
    _holdReq = false;
    _holdBlocked = false;
    taskEXIT_CRITICAL(&_holdMux);
}

void ModeManager::serviceHoldOutput() {
    const unsigned long HOLD_TIMEOUT_MS = 250;
    const unsigned long HOLD_MAX_MS     = 30000;

    taskENTER_CRITICAL(&_holdMux);
    bool holdReq        = _holdReq;
    unsigned long lastH = _lastHoldMs;
    uint8_t holdPwm     = _holdPwm;
    taskEXIT_CRITICAL(&_holdMux);

    if (holdReq && (millis() - lastH > HOLD_TIMEOUT_MS)) {
        taskENTER_CRITICAL(&_holdMux); _holdReq = false; _holdBlocked = false; taskEXIT_CRITICAL(&_holdMux);
        holdReq = false;
    }

    if (holdReq && !_holdOn) {
        _holdOn = true;
        _holdStartMs = millis();
        if (_isPlaySequenceActive) stopPlaySequence();
        if (_hwManager) _hwManager->setMosfets(holdPwm);
        Log::Info(PSTR("MODE: HOLD ON"));
    }
    if (_holdOn && (millis() - _holdStartMs > HOLD_MAX_MS)) {
        taskENTER_CRITICAL(&_holdMux); _holdReq = false; _holdBlocked = true; taskEXIT_CRITICAL(&_holdMux);
        holdReq = false;
        Log::Warn(PSTR("MODE: HOLD max time — forced OFF"));
    }

    if (!holdReq && _holdOn) {
        _holdOn = false;
        if (_hwManager) _hwManager->setMosfets(false);
        Log::Info(PSTR("MODE: HOLD OFF"));
    }
}

void ModeManager::handleScanProbePacket(uint8_t channel, uint8_t index) {
    // 측정 단계(0~2)가 아니면(측정 종료/리포트 중) 조용히 무시 — 늦게 도착한 프루브 경고 스팸 방지
    if (_currentMode != DeviceMode::MODE_RF_SCAN || _scanStepIdx >= 3) return;

    // 스캔 단계 인덱스 정합성 검증 (Ch 1 = 0, Ch 6 = 1, Ch 11 = 2)
    uint8_t expectedIdx;
    if (channel == 1) expectedIdx = 0;
    else if (channel == 6) expectedIdx = 1;
    else if (channel == 11) expectedIdx = 2;
    else return;

    if (_scanStepIdx == expectedIdx && _scanSuccessRates[expectedIdx] < 255) {
        _scanSuccessRates[expectedIdx]++;
    }
}

void ModeManager::updateModeRfScan() {
    unsigned long elapsed = millis() - _scanStepStartTime;
    uint8_t channels[] = {1, 6, 11};
    // 모든 채널 동일 체류(1800ms)로 공정 비교 (송신기가 빠르게 순회하므로 충분한 표본 확보)
    const uint32_t stepDwellLimit = 1800;

    // 1. 각 채널별 핑 측정 루프
    if (_scanStepIdx < 3) {
        if (elapsed >= stepDwellLimit) {
            _scanStepIdx++;
            _scanStepStartTime = millis();
            if (_scanStepIdx < 3) {
                uint8_t nextCh = channels[_scanStepIdx];
                esp_wifi_set_channel(nextCh, WIFI_SECOND_CHAN_NONE);

                uint8_t primaryChan = 0;
                wifi_second_chan_t secondChan = WIFI_SECOND_CHAN_NONE;
                esp_wifi_get_channel(&primaryChan, &secondChan);
                Log::Info(PSTR("MODE: RF Scan step switching to Ch %d (HW: %d)..."), nextCh, primaryChan);
            } else {
                // 전체 측정 종료 -> Ch 1로 복귀하여 분석 및 결과 도출 시작
                esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

                uint8_t primaryChan = 0;
                wifi_second_chan_t secondChan = WIFI_SECOND_CHAN_NONE;
                esp_wifi_get_channel(&primaryChan, &secondChan);
                Log::Info(PSTR("MODE: RF Scan measurements finished (HW Ch for report: %d). Analyzing data..."), primaryChan);
            }
        }
    } else {
        // 2. 스캔 완료 리포트 및 저장 단계
        if (_scanReportAttempts == 0) {
            _rfScanBestCh = 1; // 매 스캔마다 초기화 (static 잔류 방지)
            for (int i = 0; i < 3; i++) {
                Log::Info(PSTR("MODE: Ch %d probe count: %d"), channels[i], _scanSuccessRates[i]);
            }

            // [핵심] Ch 1 우선(sticky) 선택: Ch 6/11은 Ch 1보다 20% 이상 좋아야 채택.
            // 안정성 우선 — 미미한 차이로 채널을 옮겨다니지 않음. 동점/근소차는 Ch 1 유지.
            uint8_t ch1Count = _scanSuccessRates[0];
            uint8_t bestCount = ch1Count;
            for (int i = 1; i < 3; i++) {
                // _scanSuccessRates[i] > bestCount 이면서 Ch1 대비 1.2배 이상 (정수연산: *5 > ch1*6)
                if (_scanSuccessRates[i] > bestCount && (uint16_t)_scanSuccessRates[i] * 5 > (uint16_t)ch1Count * 6) {
                    bestCount = _scanSuccessRates[i];
                    _rfScanBestCh = channels[i];
                }
            }

            // 전 채널 측정이 너무 빈약하면(통신 자체 불량) 안전한 Ch 1 유지.
            const uint8_t MIN_VIABLE_PROBES = 15;
            if (bestCount < MIN_VIABLE_PROBES) {
                Log::Warn(PSTR("MODE: Scan data weak (best count %d < %d). Keeping Ch 1."), bestCount, MIN_VIABLE_PROBES);
                _rfScanBestCh = 1;
            }

            Log::Info(PSTR("MODE: AUTO CH selected -> Ch %d (awaiting TX commit)"), _rfScanBestCh);
            // [안전 핸드셰이크] 여기서 채널을 바꾸지 않음. 송신기가 리포트를 받고 '확정 신호'를
            // 되보낼 때만 전환 → 리포트 유실 시 양쪽 모두 현재 채널 유지(불일치 방지).
            _scanReportAttempts = 1;
            _lastScanReportTime = 0;            // 즉시 첫 리포트 송신
            _scanStepStartTime = millis();      // 확정 대기 타임아웃 기준
        }

        // [전환] 송신기 확정 신호를 받으면 그 채널로 전환 (양쪽 동시 이동 보장)
        if (_pendingChannelCommit) {
            _pendingChannelCommit = false;
            uint8_t c = _committedChannel;
            if (c == 1 || c == 6 || c == 11) {
                Utils::saveCommChannel(c);
                ESP_NOW_CHANNEL = c;
                Log::Info(PSTR("MODE: AUTO CH commit received -> settling on Ch %d"), c);
            }
            if (_hwManager) _hwManager->setLedPattern(LedPatternType::LED_BOOT_SUCCESS);
            switchToMode(DeviceMode::MODE_NORMAL, true);
            return;
        }

        // 확정 신호를 받을 때까지 리포트를 100ms 주기로 반복 송신 (송신기가 best 채널 인지하도록)
        if (millis() - _lastScanReportTime >= 100) {
            _lastScanReportTime = millis();
            Comm::ScanReportPacket report;
            memcpy(report.signature, Comm::kSig, 4);
            report.version = Comm::kVersion;
            report.packetType = Comm::SCAN_REPORT_PACKET;
            report.senderId = _deviceId;
            report.bestChannel = _rfScanBestCh;
            for (int i = 0; i < 3; i++) report.channelSuccessRates[i] = _scanSuccessRates[i];
            report.crc8 = Comm::crc8(reinterpret_cast<const uint8_t*>(&report), sizeof(report) - 1);
            esp_now_send(BROADCAST_ADDRESS, (const uint8_t*)&report, sizeof(report));
        }

        // 일정 시간(8초) 내 확정 신호가 없으면 → 채널 변경 없이 현재 채널 유지하고 종료(불일치 없음)
        if (millis() - _scanStepStartTime > 8000) {
            Log::Warn(PSTR("MODE: AUTO CH no commit from TX. Keeping current channel (no change)."));
            switchToMode(DeviceMode::MODE_NORMAL, true);
        }
    }
}
