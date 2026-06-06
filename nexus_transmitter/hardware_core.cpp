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
#define SOFT_START_MS       200   // 200ms 동안 서서히 켜짐

// 모터 상태 구조체 확장 (램프업 시작 시간 관리를 위해)
static struct { 
    unsigned long startTime; 
    unsigned long duration; 
    bool isError; 
    bool isActive; // 현재 모터가 켜져 있는지 여부
} motorState = { 0, 0, false, false };

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
        unsigned long currentTime = millis();
        unsigned long elapsed = currentTime - motorState.startTime;

        if (elapsed >= motorState.duration) {
            // 시간이 다 되면 끄기
            digitalWrite(VIB_MOTOR_PIN, LOW);
            motorState.isActive = false;
            motorState.startTime = 0;
            logPrintf(LogLevel::LOG_INFO, "HW: Motor STOPPED");
        }
        // 켜져 있는 동안은 아무것도 안 해도 됨 (이미 켜져 있으니까)
    }
}

void updateBatteryLevel() {
    const unsigned long BATTERY_READ_INTERVAL = 2000;
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
        
        // 통신 실패한 기기는 처리 중단
        if (rd.commStatus != COMM_ACK_RECEIVED_SUCCESS) {
            rd.isFinished = true;
            continue; 
        }
        
        if (rd.isFinished) continue;

        const DeviceSettings& settings = deviceSettings[rd.deviceID];
        
        unsigned long elapsedMs = (micros() - rd.txButtonPressSequenceMicros) / 1000;
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
                } 
                break; 
            } else {
                // 완료된 스텝 종료 진동
                if (rd.currentStepIndex == step) {
                    if (rd.playStarted && !rd.playEnded) {
                        startMotorVibration(500, false); // [진동] 종료
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
            currentMode = MODE_COMPLETION_MESSAGE;
            updateDisplay();
        }
    }
}

void handleTimerMonitor() {}

void checkExecutionAndMode() {
    unsigned long now = millis();
    
    // 1. 송신(통신) 중일 때 처리
    if (currentMode == MODE_EXECUTION && isProcessing) {
        bool allCommFinished = manageCommunication();
        
        if (allCommFinished) {
            logPrintf(LogLevel::LOG_INFO, "Communication Phase Complete.");
            
            int successCount = 0;
            for(int i=0; i<groupDeviceCount; i++) {
                if(runningDevices[i].commStatus == COMM_ACK_RECEIVED_SUCCESS) {
                    successCount++;
                } else {
                    runningDevices[i].isFinished = true;
                }
            }

            logPrintf(LogLevel::LOG_INFO, "Comm Result: %d/%d success.", successCount, groupDeviceCount);

            if (successCount > 0) {
                currentMode = MODE_TIMER_MONITOR;
            } else {
                currentMode = MODE_COMPLETION_MESSAGE;
                executionComplete = true; 
                isProcessing = false; 
                executionCompleteTime = millis();
            }
            uiState.scrollOffset = 0;
            updateDisplay();
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
            if (previousSelectedDevice == 0) currentMode = MODE_GROUP_EXECUTE_MENU;
            else currentMode = MODE_SINGLE_EXECUTE_MENU;
            
            isProcessing = false;
            executionComplete = true;
            groupDeviceCount = 0;
            updateDisplay();
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
        settings.steps[step].playSeconds++;
        if (settings.steps[step].playSeconds > MAX_PLAY_SECONDS) settings.steps[step].playSeconds = MIN_PLAY_SECONDS;
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
        settings.steps[step].playSeconds = (settings.steps[step].playSeconds == MIN_PLAY_SECONDS) ? MAX_PLAY_SECONDS : settings.steps[step].playSeconds - 1;
    } else if (currentMode == MODE_TIME_SETTING_PWM_EDIT) {
        if (settings.steps[step].pwmValue > 0) settings.steps[step].pwmValue--;
        else settings.steps[step].pwmValue = 100;
    }
}

// [핵심 수정] PWM 설정 및 소프트 스타트 시작 함수 (ESP32 v3.0 호환)
void startMotorVibration(uint16_t duration, bool isError) {
    // [비상 수정] 복잡한 PWM 설정 다 제거하고 단순 핀 모드 설정만 함
    pinMode(VIB_MOTOR_PIN, OUTPUT);
    
    motorState.startTime = millis();
    motorState.duration = duration;
    motorState.isError = isError;
    motorState.isActive = true;

    // 모터 즉시 켜기 (최대 파워)
    digitalWrite(VIB_MOTOR_PIN, HIGH);
    logPrintf(LogLevel::LOG_INFO, "HW: Motor STARTED (Digital Mode)");
}

float readBatteryVoltage() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    const int SAMPLES = 16;
    uint32_t sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sum += analogRead(BATTERY_ADC_PIN);
        delayMicroseconds(150);
    }
    uint32_t raw = sum / SAMPLES;
    const float ADC_REF = 3.3f;
    float current_voltage = (raw / 4095.0f) * ADC_REF * VOLTAGE_DIVIDER_RATIO;
    voltage_samples[sample_index++] = current_voltage;
    if (sample_index >= BATTERY_AVG_SAMPLES) {
        sample_index = 0;
        samples_filled = true;
    }
    float final_voltage;
    if (samples_filled) {
        float total_voltage = 0;
        for (int i = 0; i < BATTERY_AVG_SAMPLES; i++) total_voltage += voltage_samples[i];
        final_voltage = total_voltage / BATTERY_AVG_SAMPLES;
    } else {
        float total_voltage = 0;
        for (int i = 0; i < sample_index; i++) total_voltage += voltage_samples[i];
        final_voltage = (sample_index > 0) ? (total_voltage / sample_index) : 0;
    }
    static unsigned long lastLogTime = 0;
    if (millis() - lastLogTime >= 5000) {
        logPrintf(LogLevel::LOG_INFO, "Battery: %.3fV", final_voltage);
        lastLogTime = millis();
    }
    return final_voltage;
}

int getBatteryPercentage(float voltage) {
    // 3.75V 이상이면 4칸(완충) 표시
    if (voltage >= 3.75f) return 4;
    if (voltage >= 3.60f) return 3;
    if (voltage >= 3.45f) return 2;
    if (voltage >= 3.30f) return 1;
    return 0;
}

void prepareForExecution() {
    isProcessing = true;
    executionComplete = false;
    currentMode = MODE_EXECUTION;
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
    rd.commStatus = COMM_PENDING_RTT_REQUEST;
    rd.sendAttempts = 0;
    rd.lastPacketSendTime = 0;
    rd.isFinished = false;
    
    logPrintf(LogLevel::LOG_INFO, "HW: Single exec ID %d start.", deviceID);
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
            rd.commStatus = COMM_PENDING_RTT_REQUEST;
            rd.sendAttempts = 0;
            rd.lastPacketSendTime = 0;
            rd.isFinished = false;
            groupDeviceCount++;
            logPrintf(LogLevel::LOG_INFO, "HW: Group exec add ID %d.", id);
        }
    }
    
    if (groupDeviceCount == 0) {
        logPrintf(LogLevel::LOG_INFO, "HW: No valid devices in group.");
        isProcessing = false;
        currentMode = MODE_GROUP_EXECUTE_MENU;
    }
}