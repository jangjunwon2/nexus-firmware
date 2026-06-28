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

    // [NEW] RF 환경 자동 스캔 관련 명령 처리
    void handleScanStartCommand();
    void handleScanProbePacket(uint8_t channel, uint8_t index);
    void handleChannelCommit(uint8_t channel); // [NEW] 송신기 채널 확정 신호 수신(콜백)
    void handleHold(uint8_t pwm, uint8_t active); // [NEW] 홀드 패킷 수신(콜백) — 플래그만 세팅
    void cancelPlaySequence(); // [NEW] CANCEL_COMMAND 수신 시 즉시 시퀀스 중단

private:
    HardwareManager* _hwManager;
    CommManager* _commManager;
    WebManager* _webManager;

    DeviceMode _currentMode;
    uint8_t _deviceId;
    SemaphoreHandle_t _modeSwitchMutex;
    volatile uint32_t _currentCommandId;
    uint32_t _lastAckedTxMicros; // 중복 ACK 방지 (마지막으로 ACK한 FIRE txMicros)

    // [핵심] 지연 ACK — 송신기 FIRE 버스트(반이중 송신) 중엔 수신 불가하므로,
    // 버스트가 끝날 시간(~12ms)을 두고 ACK를 보내야 송신기가 받을 수 있음.
    portMUX_TYPE _ackMux;
    volatile bool _ackPending;
    uint32_t _ackOrigTxMicros;
    volatile unsigned long _ackQueuedAt;

    ExecutionStep _executionPlan[MAX_EXECUTION_STEPS];
    uint8_t _planStepCount;
    uint8_t _currentStepIndex;

    IdSetState _idSetState;
    uint8_t _temporaryId;
    unsigned long _idSetLastInputTime;

    volatile bool _isPlaySequenceActive;
    bool _isManualOperationActive;
    bool _isDelayPhase;
    unsigned long _phaseStartMs;
    uint32_t _phaseDuration;
    unsigned long _lastExecButtonActionTime;

    unsigned long _lastWebApiActivityTime;
    bool _updateDownloaded;
    bool _idBlinkPatternStarted;
    uint8_t _previousDeviceId;
    
    // [NEW] 페어링 대기열 타임아웃용 타이머
    unsigned long _pairingStartTime;

    // [플럭스 케이스 고유] 타이머 동작 시간 — 리모컨/테스트로 갱신, NVS 저장됨
    uint32_t _timedRunDurationMs;
    uint32_t _lastRemoteTestPlayMs;

    uint32_t _firstStepOverrideDelayMs; // elapsed 보정 후 ms 정밀도 유지용

    // [NEW] RF 자동 환경 분석 관련 내부 측정 변수들
    uint8_t _scanSuccessRates[3];
    unsigned long _scanStepStartTime;
    uint8_t _scanStepIdx;
    unsigned long _lastScanReportTime;
    uint8_t _scanReportAttempts;
    volatile bool _pendingScanStart;       // 콜백→메인루프 스캔 전환
    volatile bool _pendingChannelCommit;   // 송신기 확정 신호 수신 플래그
    volatile uint8_t _committedChannel;     // 확정된 채널
    uint8_t _rfScanBestCh;

    portMUX_TYPE _manualRunMux;
    volatile bool _pendingManualRun;
    volatile uint32_t _pendingManualDelayMs;
    volatile uint32_t _pendingManualPlayMs;

    volatile bool _pendingTimedRunSave;
    volatile uint32_t _pendingTimedRunMs;

    // [NEW] 홀드(누르는 동안 지속) 수신 상태. 콜백은 플래그만, 출력은 update()에서.
    portMUX_TYPE _holdMux;
    volatile bool _holdReq;
    volatile uint8_t _holdPwm;
    volatile unsigned long _lastHoldMs;
    bool _holdOn;
    volatile bool _holdBlocked;
    unsigned long _holdStartMs;
    void serviceHoldOutput();

    void enterModeLogic(DeviceMode mode);
    void exitModeLogic(DeviceMode mode);
    void attemptAutoConnection();

    void updateModeNormal();
    void updateModeIdBlink();
    void updateModeIdSet();
    void updateModeWifi();
    void updateModeExitWifi();
    void updateModePairing(); // [NEW] 페어링 모드 업데이트
    void updateModeRfScan();  // [NEW] RF 자동 스캔 모드 업데이트
    void updatePlaySequence();  

    void startPlaySequence(const ExecutionStep* steps, uint8_t stepCount, uint32_t elapsedSinceButtonUs);
    void stopPlaySequence();
    void startNextStep();
    
    void incrementTemporaryId();
    void finalizeIdSelection();
    const char* getModeName(DeviceMode mode) const;

    void startManualOperation();
    void stopManualOperation();
};

#endif // MODE_H