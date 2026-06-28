// transmitter_s3/hardware_core.cpp

#include "hardware_core.h"
#include "config_t.h"
#include "hardware_buttons.h"
#include "hardware_display.h"
#include "espnow_t.h"
#include "utils_t.h"
#include <algorithm>
#include "hardware_init.h"
#include "WiFi.h"
#include "esp_now.h"
#include "driver/rtc_io.h"

// [Soft Start 설정]
// ESP32 v3.0 이상에서는 채널(Channel)을 직접 지정하지 않고 핀을 제어합니다.
#define MOTOR_PWM_FREQ      5000
#define MOTOR_PWM_RES       8

// 모터 상태 구조체 확장 (램프업 시작 시간 관리를 위해)
static struct { 
    unsigned long startTime; 
    unsigned long duration; 
    bool isError; 
    bool isActive; // 현재 모터가 켜져 있는지 여부
} motorState = { 0, 0, false, false };

// ── [NEW] 홀드(누르는 동안 지속) 세션 ──────────────────────────────────────────
// STYLE=HOLD에서 PLAY를 누르는 동안 킵얼라이브(HOLD 패킷)를 주기 송신, 떼면 OFF.
#define HOLD_SEND_INTERVAL_MS 60   // 송신 주기(수신기 타임아웃 ~250ms보다 충분히 짧게)
#define HOLD_MAX_MS           (SMOKE_SINGLE_MAX_S * 1000UL) // 수신기와 동일 15초 안전 제한
static bool holdActive = false;
static unsigned long holdLastSend = 0;
static unsigned long holdStartMs = 0;

static void sendHoldToTargets(bool group, uint8_t active) {
    if (group) {
        for (uint8_t id = 1; id <= MAX_DEVICES; id++) {
            if ((deviceSettings[id].groupMembershipBitmask >> (selectedGroupNum - 1)) & 0x01) {
                const DeviceSettings& d = deviceSettings[id];
                uint8_t step = (uiState.selectedStep < d.stepCount) ? uiState.selectedStep : 0;
                sendHoldCommand(id, (uint8_t)d.machineType, d.steps[step].pwmValue, active);
            }
        }
    } else {
        const DeviceSettings& d = deviceSettings[selectedDevice];
        uint8_t step = (uiState.selectedStep < d.stepCount) ? uiState.selectedStep : 0;
        sendHoldCommand(selectedDevice, (uint8_t)d.machineType, d.steps[step].pwmValue, active);
    }
}

static void forceHoldOff(bool group) {
    holdActive = false;
    holdStartMs = 0;
    for (int i = 0; i < 3; i++) { sendHoldToTargets(group, 0); delay(2); }
}

// 매 루프 호출. down=ENTER 눌림 유지 여부. group=그룹 홀드 여부.
void serviceHold(bool down, bool group) {
    if (down) {
        if (!holdActive) {
            holdActive = true;
            holdStartMs = millis();
        }
        if (millis() - holdStartMs >= HOLD_MAX_MS) {
            forceHoldOff(group); // 최대 지속시간 초과 → 강제 OFF
            return;
        }
        if (millis() - holdLastSend >= HOLD_SEND_INTERVAL_MS) {
            holdLastSend = millis();
            sendHoldToTargets(group, 1); // 킵얼라이브 ON
        }
    } else if (holdActive) {
        forceHoldOff(group); // 버튼 뗌 → OFF
    }
}
// ──────────────────────────────────────────────────────────────────────────────

static float voltage_samples[BATTERY_AVG_SAMPLES];
static int sample_index = 0;
static bool samples_filled = false;
static unsigned long lastBatteryReadTime = 0;
extern unsigned long lastEspNowTxTime;

void updateButtons() {
    buttonUp.update();
    buttonDown.update();
    buttonBack.update();
    buttonEnter.update();
}

void updateVibrationMotor() {
    if (motorState.isActive) {
        unsigned long elapsed = millis() - motorState.startTime;
        if (elapsed >= motorState.duration) {
            ledcWrite(VIB_MOTOR_PIN, 0);
            ledcDetach(VIB_MOTOR_PIN);
            pinMode(VIB_MOTOR_PIN, OUTPUT);
            digitalWrite(VIB_MOTOR_PIN, LOW);
            motorState.isActive = false;
            motorState.startTime = 0;
            logPrintf(LogLevel::LOG_INFO, "HW: Motor STOPPED");
        }
    }
}

void updateBatteryLevel() {
    const unsigned long BATTERY_READ_INTERVAL = 2000;
    if (motorState.isActive) return;                   // D: 모터 동작 중 측정 제외
    if (millis() - lastEspNowTxTime < 100) return;
    if (millis() - lastBatteryReadTime >= BATTERY_READ_INTERVAL) {
        batteryVoltage = readBatteryVoltage();
        batteryPercentage = getBatteryPercentage(batteryVoltage);
        lastBatteryReadTime = millis();
    }
}

void processBackgroundTimer() {
    if (!isProcessing) return; 

    bool allDevicesFinished = true;
    
    for (int i = 0; i < groupDeviceCount; i++) {
        RunningDevice& rd = runningDevices[i];
        
        // [NEW] 비동기 ACK 대기 + 자동 재시도 처리
        // RF 유실이 잦아도 신뢰성 확보: ACK 미수신 시 짧은 간격으로 FIRE 재송신.
        // 수신기는 버튼 누른 시각으로 중복을 판별하므로 재시도는 중복 발사를 일으키지 않음.
        taskENTER_CRITICAL(&rdMux);
        CommStatus rdStatus = rd.commStatus;
        taskEXIT_CRITICAL(&rdMux);

        if (rdStatus == COMM_AWAITING_ACK) {
            const unsigned long FIRE_RETRY_INTERVAL_MS = RETRY_INTERVAL_MS;
            const uint8_t MAX_FIRE_ATTEMPTS = MAX_SEND_ATTEMPTS;
            taskENTER_CRITICAL(&rdMux);
            unsigned long lastSendTime = rd.lastPacketSendTime;
            uint8_t attempts = rd.sendAttempts;
            taskEXIT_CRITICAL(&rdMux);
            if (millis() - lastSendTime > FIRE_RETRY_INTERVAL_MS) {
                if (attempts < MAX_FIRE_ATTEMPTS) {
                    sendFireBurst(rd);
                    taskENTER_CRITICAL(&rdMux);
                    rd.sendAttempts++;
                    taskEXIT_CRITICAL(&rdMux);
                    logPrintf(LogLevel::LOG_INFO, "BG TIMER: Device %d no ACK, retry %d/%d.",
                              rd.deviceID, rd.sendAttempts, MAX_FIRE_ATTEMPTS);
                } else {
                    taskENTER_CRITICAL(&rdMux);
                    if (rd.commStatus == COMM_AWAITING_ACK) {
                        rd.commStatus = COMM_FAILED_NO_ACK;
                        rd.isFinished = true;
                    }
                    taskEXIT_CRITICAL(&rdMux);
                    logPrintf(LogLevel::LOG_WARN, "BG TIMER: Device %d FAILED after %d attempts. (TX batt %.2fV)",
                              rd.deviceID, MAX_FIRE_ATTEMPTS, batteryVoltage);
                }
            }
            allDevicesFinished = false;
            continue;
        }

        // 통신 실패한 기기는 처리 중단
        if (rd.commStatus == COMM_FAILED_NO_ACK) {
            rd.isFinished = true;
            continue; 
        }
        
        if (rd.isFinished) continue;

        const DeviceSettings& settings = deviceSettings[rd.deviceID];
        
        unsigned long elapsedMs = millis() - rd.startTimeMs;
        unsigned long currentStepStartMs = 0;
        bool isDeviceActiveInStep = false;

        for (int step = 0; step < settings.stepCount; step++) {
            unsigned long rawDelayMs = (unsigned long)settings.steps[step].delayMinutes * 60000 + settings.steps[step].delaySeconds * 1000;
            unsigned long delayMs = rawDelayMs; 
            unsigned long playMs = (unsigned long)settings.steps[step].playSeconds * 1000;
            
            unsigned long playStartMs = currentStepStartMs + delayMs;
            unsigned long stepEndMs = playStartMs + playMs;

            // 현재 시간이 이 스텝 범위 안에 있다면 (Delay 또는 Play 구간)
            if (elapsedMs < stepEndMs) {
                isDeviceActiveInStep = true;

                // 스텝 변경 감지
                if (rd.currentStepIndex != step) {
                    rd.currentStepIndex = step; 
                    rd.playStarted = false;     
                    rd.playEnded = false;       
                    logPrintf(LogLevel::LOG_INFO, "BG TIMER: Device %d Entered Step %d", rd.deviceID, step + 1);
                }

                if (elapsedMs >= playStartMs) {
                    // Play 구간
                    if (!rd.playStarted) {
                        startMotorVibration(500, false); // [진동] 시작
                        rd.playStarted = true;
                        rd.playEnded = false;
                        logPrintf(LogLevel::LOG_INFO, "BG TIMER: Device %d Step %d Playing", rd.deviceID, step+1);
                    }
                    // 완료 0.5초 전 알림 진동
                    if (rd.playStarted && !rd.playEnded && elapsedMs >= stepEndMs - 500) {
                        startMotorVibration(500, false); // [진동] 완료 예고
                        rd.playEnded = true;
                        logPrintf(LogLevel::LOG_INFO, "BG TIMER: Device %d Step %d PreEnd Vib", rd.deviceID, step+1);
                    }
                }
                break;
            } else {
                // 완료된 스텝 처리 (엣지케이스 폴백)
                if (rd.currentStepIndex == step) {
                    if (rd.playStarted && !rd.playEnded) {
                        startMotorVibration(500, false); // [진동] 종료 (폴백)
                        rd.playEnded = true;
                        logPrintf(LogLevel::LOG_INFO, "BG TIMER: Device %d Step %d Play Finished", rd.deviceID, step+1);
                    }
                }
            }
            currentStepStartMs = stepEndMs;
        }

        if (!isDeviceActiveInStep && elapsedMs >= currentStepStartMs) {
            rd.isFinished = true;
        } else {
            allDevicesFinished = false;
        }
    }

    if (allDevicesFinished) {
        logPrintf(LogLevel::LOG_INFO, "BG TIMER: All finished.");
        executionComplete = true;
        isProcessing = false;
        executionCompleteTime = millis();

        if (currentMode == MODE_TIMER_MONITOR) {
            changeMode(MODE_COMPLETION_MESSAGE);
        }
        updateDisplay();
    }
}

void handleTimerMonitor() {}

void checkExecutionAndMode() {
    unsigned long now = millis();

    // 0. AUTO CH(채널 최적화) 모드는 백그라운드 태스크(rfScanTask)가 처리 → 실행 로직 건너뜀
    if (currentMode == MODE_RF_SCAN) {
        return;
    }

    // 0-1. 페어링(CLONE_TX) 모드: 양쪽이 페어링 모드이기만 하면 한 번에 붙도록 자동 반복 송신.
    // (수동 ENTER 타이밍을 맞출 필요 없음 → 페어링 창과 자동으로 겹침)
    if (currentMode == MODE_CLONE_TX) {
        static unsigned long lastAutoClone = 0;
        if (now - lastAutoClone >= 1500) { // 1.5초마다 자동 재송신 (한 번에 붙을 확률↑)
            lastAutoClone = now;
            sendCloneAnnounce();
            cloneTxSentAt = now;
        }
        return;
    }

    // 1. 송신(통신) 중일 때 처리
    if (currentMode == MODE_EXECUTION && isProcessing) {
        bool allCommFinished = manageCommunication();
        
        if (allCommFinished) {
            logPrintf(LogLevel::LOG_INFO, "Communication Phase Complete. Entering Monitor Mode (Awaiting ACKs asynchronously).");
            
            // [NEW] 버스트 전송 후 대기 없이 즉각 타이머 모니터 모드로 전환
            uiState.scrollOffset = 0;
            changeMode(MODE_TIMER_MONITOR);
        }
    }
    
    // 2. 백그라운드 타이머 로직
    if (isProcessing && currentMode != MODE_EXECUTION) {
        processBackgroundTimer();
    }
    
    // 3. 완료 메시지 타임아웃
    if (currentMode == MODE_COMPLETION_MESSAGE) {
        if (now - executionCompleteTime >= 1000) {
            logPrintf(LogLevel::LOG_INFO, "Completion screen timed out.");
            isProcessing = false;
            executionComplete = true;
            groupDeviceCount = 0;
            if (previousSelectedDevice == 0) changeMode(MODE_GROUP_EXECUTE_MENU);
            else changeMode(MODE_SINGLE_EXECUTE_MENU);
        }
    }

    // 4. 취소 메시지 타임아웃 (1.5초 후 홈으로)
    if (currentMode == MODE_CANCEL_MESSAGE) {
        if (now - executionCompleteTime >= 1500) {
            isProcessing = false;
            groupDeviceCount = 0;
            changeMode(MODE_HOME_MENU);
            modeHistory.clear();
            uiState.menuCursor = 0;
        }
    }
}

void increaseTimerValue() {
    auto& settings = deviceSettings[selectedDevice];
    uint8_t step = uiState.selectedStep;
    if (currentMode == MODE_TIME_SETTING_DELAY_EDIT) {
        if (uiState.adjustingUnit == UNIT_MINUTES) settings.steps[step].delayMinutes = (settings.steps[step].delayMinutes + 1) % (MAX_DELAY_MINUTES + 1);
        else settings.steps[step].delaySeconds = (settings.steps[step].delaySeconds + 1) % (MAX_DELAY_SECONDS + 1);
    } else if (currentMode == MODE_TIME_SETTING_PLAY_EDIT) {
        const auto& s = deviceSettings[selectedDevice];
        uint8_t playMax = (s.machineType == TYPE_SMOKE)
            ? ((s.stepCount == 1) ? SMOKE_SINGLE_MAX_S : SMOKE_STEP_MAX_S)
            : MAX_PLAY_SECONDS;
        settings.steps[step].playSeconds++;
        if (settings.steps[step].playSeconds > playMax) settings.steps[step].playSeconds = MIN_PLAY_SECONDS;
    } else if (currentMode == MODE_TIME_SETTING_PWM_EDIT) {
        if (settings.steps[step].pwmValue < 100) settings.steps[step].pwmValue++;
        else settings.steps[step].pwmValue = 0;
    }
}

void decreaseTimerValue() {
    auto& settings = deviceSettings[selectedDevice];
    uint8_t step = uiState.selectedStep;
    if (currentMode == MODE_TIME_SETTING_DELAY_EDIT) {
        if (uiState.adjustingUnit == UNIT_MINUTES) settings.steps[step].delayMinutes = (settings.steps[step].delayMinutes == 0) ? MAX_DELAY_MINUTES : settings.steps[step].delayMinutes - 1;
        else settings.steps[step].delaySeconds = (settings.steps[step].delaySeconds == 0) ? MAX_DELAY_SECONDS : settings.steps[step].delaySeconds - 1;
    } else if (currentMode == MODE_TIME_SETTING_PLAY_EDIT) {
        const auto& s = deviceSettings[selectedDevice];
        uint8_t playMax = (s.machineType == TYPE_SMOKE)
            ? ((s.stepCount == 1) ? SMOKE_SINGLE_MAX_S : SMOKE_STEP_MAX_S)
            : MAX_PLAY_SECONDS;
        settings.steps[step].playSeconds = (settings.steps[step].playSeconds == MIN_PLAY_SECONDS) ? playMax : settings.steps[step].playSeconds - 1;
    } else if (currentMode == MODE_TIME_SETTING_PWM_EDIT) {
        if (settings.steps[step].pwmValue > 0) settings.steps[step].pwmValue--;
        else settings.steps[step].pwmValue = 100;
    }
}

void startMotorVibration(uint16_t duration, bool isError) {
    if (!vibrationEnabled) return;
    if (execStyle == STYLE_HOLD) return; // 홀드 모드에서는 진동 없음
    ledcAttach(VIB_MOTOR_PIN, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
    ledcWrite(VIB_MOTOR_PIN, VIB_MOTOR_STRENGTH); // blocking delay 없이 즉시 전진폭

    motorState.startTime = millis();
    motorState.duration = duration;
    motorState.isError = isError;
    motorState.isActive = true;
    logPrintf(LogLevel::LOG_INFO, "HW: Motor STARTED");
}

float readBatteryVoltage() {
    // A: analogReadMilliVolts() — eFuse 교정값 자동 적용, ADC 비선형 오차 제거
    const int SAMPLES = 16;
    uint32_t sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sum += analogReadMilliVolts(BATTERY_ADC_PIN);
        delayMicroseconds(150);
    }
    float pin_mv = sum / (float)SAMPLES;
    float current_voltage = (pin_mv / 1000.0f) * VOLTAGE_DIVIDER_RATIO;

    voltage_samples[sample_index++] = current_voltage;
    if (sample_index >= BATTERY_AVG_SAMPLES) {
        sample_index = 0;
        samples_filled = true;
    }
    float final_voltage;
    if (samples_filled) {
        float total = 0;
        for (int i = 0; i < BATTERY_AVG_SAMPLES; i++) total += voltage_samples[i];
        final_voltage = total / BATTERY_AVG_SAMPLES;
    } else {
        float total = 0;
        for (int i = 0; i < sample_index; i++) total += voltage_samples[i];
        final_voltage = (sample_index > 0) ? (total / sample_index) : 0;
    }
    return final_voltage;
}

int getBatteryPercentage(float voltage) {
    // B: LiPo 실제 방전 커브 기반 선형 보간
    static const float voltTable[] = { 3.00f, 3.27f, 3.53f, 3.67f, 3.79f, 3.87f, 3.98f, 4.20f };
    static const float capTable[]  = {  0.0f,  5.0f, 10.0f, 30.0f, 50.0f, 75.0f, 90.0f, 100.0f };
    const int N = 8;

    float cap = 0.0f;
    if (voltage <= voltTable[0]) {
        cap = 0.0f;
    } else if (voltage >= voltTable[N - 1]) {
        cap = 100.0f;
    } else {
        for (int i = 0; i < N - 1; i++) {
            if (voltage < voltTable[i + 1]) {
                float t = (voltage - voltTable[i]) / (voltTable[i + 1] - voltTable[i]);
                cap = capTable[i] + t * (capTable[i + 1] - capTable[i]);
                break;
            }
        }
    }

    // C: 히스테리시스 — 올라갈 때와 내려갈 때 다른 임계값으로 경계 흔들림 방지
    static int lastBars = -1;
    if (lastBars < 0) {
        if      (cap >= 75.0f) lastBars = 4;
        else if (cap >= 50.0f) lastBars = 3;
        else if (cap >= 25.0f) lastBars = 2;
        else if (cap >= 10.0f) lastBars = 1;
        else                   lastBars = 0;
        return lastBars;
    }

    // 상향: 기준 +5% 초과해야 올라감
    if      (lastBars < 4 && cap >= 80.0f) lastBars = 4;
    else if (lastBars < 3 && cap >= 55.0f) lastBars = 3;
    else if (lastBars < 2 && cap >= 30.0f) lastBars = 2;
    else if (lastBars < 1 && cap >= 15.0f) lastBars = 1;
    // 하향: 기준 -5% 이하여야 내려감
    else if (lastBars > 3 && cap <  70.0f) lastBars = 3;
    else if (lastBars > 2 && cap <  45.0f) lastBars = 2;
    else if (lastBars > 1 && cap <  20.0f) lastBars = 1;
    else if (lastBars > 0 && cap <   5.0f) lastBars = 0;

    return lastBars;
}

void prepareForExecution() {
    isProcessing = true;
    executionComplete = false;
    uiState.scrollOffset = 0;
    for(int i=0; i<MAX_GROUP_DEVICES; i++) {
        runningDevices[i] = {};
        runningDevices[i].currentStepIndex = -1;
    }
    logPrintf(LogLevel::LOG_INFO, "HW: Execution prepared. Statuses reset.");
}

void startSingleExecution(uint8_t deviceID, unsigned long buttonPressTime) {
    if (deviceID < 1 || deviceID > MAX_DEVICES || !deviceSettings[deviceID].isValid()) return;
    prepareForExecution();
    previousSelectedDevice = deviceID;
    groupDeviceCount = 1;

    RunningDevice& rd = runningDevices[0];
    rd.deviceID = deviceID;
    rd.txButtonPressSequenceMicros = buttonPressTime;
    rd.startTimeMs = millis();
    rd.commStatus = COMM_PENDING_FIRE;
    rd.sendAttempts = 0;
    rd.lastPacketSendTime = 0;
    rd.isFinished = false;
    rd.currentStepIndex = -1;
    rd.playStarted = false;
    rd.playEnded = false;

    logPrintf(LogLevel::LOG_INFO, "HW: Single exec ID %d start.", deviceID);
    changeMode(MODE_EXECUTION);
}

void startGroupExecution(unsigned long buttonPressTime) {
    prepareForExecution();
    groupDeviceCount = 0;
    previousSelectedDevice = 0;
    
    for (uint8_t id = 1; id <= MAX_DEVICES; id++) {
        if (groupDeviceCount >= MAX_GROUP_DEVICES) break;
        if (((deviceSettings[id].groupMembershipBitmask >> (selectedGroupNum - 1)) & 0x01) && deviceSettings[id].isValid()) {
            RunningDevice& rd = runningDevices[groupDeviceCount];
            rd.deviceID = id;
            rd.txButtonPressSequenceMicros = buttonPressTime;
            rd.startTimeMs = millis();
            rd.commStatus = COMM_PENDING_FIRE;
            rd.sendAttempts = 0;
            rd.lastPacketSendTime = 0;
            rd.isFinished = false;
            rd.currentStepIndex = -1;
            rd.playStarted = false;
            rd.playEnded = false;
            groupDeviceCount++;
            logPrintf(LogLevel::LOG_INFO, "HW: Group exec add ID %d.", id);
        }
    }
    
    if (groupDeviceCount == 0) {
        logPrintf(LogLevel::LOG_INFO, "HW: No valid devices in group.");
        isProcessing = false;
        changeMode(MODE_GROUP_EXECUTE_MENU);
    } else {
        changeMode(MODE_EXECUTION);
    }
}