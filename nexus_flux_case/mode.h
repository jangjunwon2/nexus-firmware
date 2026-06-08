#pragma once
#ifndef MODE_H
#define MODE_H

#include "config.h"
#include "utils.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class HardwareManager;
class CommManager;
class WebManager;

enum class IdSetState {
    IDLE,
    ENTERED,
    AWAITING_INPUT,
    CONFIRMING_ON,
    CONFIRMING_BLINK
};

class ModeManager {
public:
    ModeManager(HardwareManager* hwManager, CommManager* commManager, WebManager* webManager);
    
    void begin();
    void update();

    void handleButtonEvent(ButtonEventType event);
    void handleEspNowCommand(const uint8_t* senderMac, const Comm::CommPacket& pkt, uint32_t rxTime);
    void triggerManualRun(uint32_t delayMs, uint32_t playMs);
    void switchToMode(DeviceMode newMode, bool forceSwitch = false);
    
    DeviceMode getCurrentMode() const;
    const char* getCurrentModeName() const;

    void recordWebApiActivity();
    void exitWifiMode();
    void updateDeviceId(uint8_t newId, bool fromWeb = false);
    void setUpdateDownloaded(bool downloaded);
    void applyUpdateAndReboot();

    // [NEW] 페어링 성공 시 통신 매니저에서 호출
    void notifyPairingSuccess();

private:
    HardwareManager* _hwManager;
    CommManager* _commManager;
    WebManager* _webManager;

    DeviceMode _currentMode;
    uint8_t _deviceId;
    SemaphoreHandle_t _modeSwitchMutex;
    uint32_t _currentCommandId;

    ExecutionStep _executionPlan[MAX_EXECUTION_STEPS];
    uint8_t _planStepCount;
    uint8_t _currentStepIndex;

    IdSetState _idSetState;
    uint8_t _temporaryId;
    unsigned long _idSetLastInputTime;

    bool _isPlaySequenceActive;
    bool _isManualOperationActive;
    bool _isDelayPhase;
    unsigned long _phaseEndTime;
    unsigned long _lastExecButtonActionTime;

    unsigned long _lastWebApiActivityTime;
    bool _updateDownloaded;
    bool _idBlinkPatternStarted;
    uint8_t _previousDeviceId;
    
    // [NEW] 페어링 대기열 타임아웃용 타이머
    unsigned long _pairingStartTime;

    uint32_t _timedRunDurationMs;   // EXEC 버튼 1회 누를 시 동작 시간 (ms)
    uint32_t _lastRemoteTestPlayMs; // 리모컨/테스트에서 마지막 설정된 play 시간 (ms)

    void enterModeLogic(DeviceMode mode);
    void exitModeLogic(DeviceMode mode);
    void attemptAutoConnection();

    void updateModeNormal();
    void updateModeIdBlink();
    void updateModeIdSet();
    void updateModeWifi();
    void updateModeExitWifi();
    void updateModePairing(); // [NEW] 페어링 모드 업데이트 루프
    void updatePlaySequence(); 

    void startPlaySequence(const ExecutionStep* steps, uint8_t stepCount, uint32_t rttUs, uint32_t rxProcUs);
    void stopPlaySequence();
    void startNextStep();
    
    void incrementTemporaryId();
    void finalizeIdSelection();
    const char* getModeName(DeviceMode mode) const;

    void startManualOperation();
    void stopManualOperation();
};

#endif // MODE_H