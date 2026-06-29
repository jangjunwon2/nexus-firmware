// transmitter/hardware_buttons.cpp

#include "config_t.h"
#include "hardware_buttons.h"
#include "hardware_core.h"
#include "hardware_display.h"
#include "ui_text_t.h"
#include "hardware_init.h"
#include <algorithm>
#include "ota_manager.h"
#include "espnow_t.h"
#include "driver/rtc_io.h"
#include "utils_t.h"

unsigned long cloneTxSentAt = 0;

void changeMode(Mode newMode) {
    if (currentMode != newMode) {
        logPrintf(LogLevel::LOG_INFO, "[STATE] Mode Changed: %s -> %s", getModeString(currentMode), getModeString(newMode));
        currentMode = newMode;
        modeEnteredAt = millis();
        // [FIX] 모드 전환 시 미소비 버튼 입력 초기화 → 이전 모드의 눌림이 새 모드로 누출되는 것 방지.
        buttonUp.resetPressCount();
        buttonDown.resetPressCount();
        buttonEnter.resetPressCount();
        buttonBack.resetPressCount();
        // [NEW] SPARE COPY 진입 시 이전 수신 상태 초기화 (재진입 안전)
        if (newMode == MODE_CLONE_RX) resetCloneRxState();
        updateDisplay();
    }
}

void navigateBack() {
    if (!modeHistory.empty()) {
        Mode previousMode = currentMode;
        Mode targetMode = modeHistory.back();
        modeHistory.pop_back();

        adjustingValue = false;
        if (targetMode == MODE_HOME_MENU) uiState.menuCursor = 0;

        bool wasInOtaMode = isOtaMode(previousMode);
        bool isStillInOtaMode = isOtaMode(targetMode);

        if (wasInOtaMode && !isStillInOtaMode) {
            shutdownWifi();
            if (!initEspNow()) {
                targetMode = MODE_ERROR;
            }
        }
        changeMode(targetMode);
    }
}

Button::Button(uint8_t p) : pin(p), pressed(false), state(HIGH), lastState(HIGH), lastDebounceTime(0), lastPressTime(0), debounceDelay(BUTTON_DEBOUNCE_TIME), lastActionTime(0), isHolding(false), countInterval(BUTTON_INITIAL_INTERVAL), pressCount(0) {}
void Button::begin() { pinMode(pin, INPUT_PULLUP); }
void Button::update() {
    bool currentState = digitalRead(pin);
    unsigned long now = millis();
    if (currentState != lastState) lastDebounceTime = now;
    if (now - lastDebounceTime >= debounceDelay) {
        if (currentState != state) {
            state = currentState;
            if (state == LOW) {
                lastPressTime = now;
                pressed = true;
                isHolding = false;
                pressCount = 0;
                countInterval = BUTTON_INITIAL_INTERVAL;
                lastActionTime = now;
            } else {
                isHolding = false;
            }
        }
    }
    if (state == LOW && !isHolding && (now - lastPressTime >= BUTTON_HOLD_TIME)) {
        isHolding = true;
        lastActionTime = now;
    }
    if (state == LOW && isHolding && (now - lastActionTime >= countInterval)) {
        lastActionTime = now;
        pressCount++;
        if (pressCount >= 5) countInterval = BUTTON_FAST_INTERVAL;
        else if (pressCount >= 3) countInterval = BUTTON_SLOW_INTERVAL;
        pressed = true;
    }
    lastState = currentState;
}
bool Button::isPressed() { if(pressed){ pressed = false; return true; } return false; }
bool Button::checkHold() { return isHolding; }
bool Button::shouldCount() {
    if (isHolding && pressed) {
        pressed = false;
        return true;
    }
    return false;
}
void Button::resetPressCount() { pressCount = 0; countInterval = BUTTON_INITIAL_INTERVAL; isHolding = false; pressed = false; }


void handleButtons() {
    static unsigned long lastButtonCheckTime = 0;
    if (millis() - lastButtonCheckTime < 10) return;
    lastButtonCheckTime = millis();

    handleOtaLoop();

    switch (currentMode) {
        case MODE_HOME_MENU:            handleHomeMenuButtons(); break;
        case MODE_GROUP_EXECUTE_MENU:   handleGroupExecuteMenuButtons(); break;
        case MODE_SINGLE_EXECUTE_MENU:  handleSingleExecuteMenuButtons(); break;
        case MODE_GROUP_SETTING_OVERVIEW: handleGroupSettingOverviewButtons(); break;
        case MODE_GROUP_SETTING_DETAIL: handleGroupSettingDetailButtons(); break;
        case MODE_TIME_SETTING_OVERVIEW: handleTimeSettingOverviewButtons(); break;
        case MODE_TIME_SETTING_DELAY_EDIT: handleTimeSettingDelayEditButtons(); break;
        case MODE_TIME_SETTING_PLAY_EDIT: handleTimeSettingPlayEditButtons(); break;
        case MODE_TIME_SETTING_PWM_EDIT: handleTimeSettingPwmEditButtons(); break;
        case MODE_UPDATE_PAGE:          handleUpdatePageButtons(); break;
        case MODE_OTA_CONFIRM:          handleOtaConfirmButtons(); break;
        case MODE_OTA_WIFI_AP:          handleApModeButtons(); break;
        case MODE_OTA_ERROR:            handleOtaErrorButtons(); break;
        case MODE_EXECUTION:            handleExecutionModeButtons(); break;
        case MODE_TIMER_MONITOR:        handleTimerMonitorButtons(); break;
        case MODE_COMPLETION_MESSAGE:   handleCompletionModeButtons(); break;
        case MODE_CANCEL_MESSAGE:       handleCancelMessageButtons(); break;
        
        // [NEW] 복제 모드 연결
        case MODE_CLONE_TX:             handleCloneTxButtons(); break;
        case MODE_CLONE_RX:             handleCloneRxButtons(); break;

        // [NEW] RF 자동 환경 분석 스캔 모드
        case MODE_RF_SCAN:              handleRfScanButtons(); break;

        // [NEW] 언어 선택 모드
        case MODE_LANGUAGE_SETTING:     handleLanguageMenuButtons(); break;

        // [NEW] 그룹 멤버 편집 모드
        case MODE_GROUP_MEMBERS:        handleGroupMembersButtons(); break;

        case MODE_OTA_SUCCESS:          handleOtaSuccessButtons(); break;
        case MODE_BOOT:
        case MODE_OTA_SCANNING:
        case MODE_OTA_DOWNLOADING:
        case MODE_OTA_UPDATING:
        case MODE_ERROR:
            break;
        case MODE_OTA_CONNECTING:
        case MODE_OTA_CHECKING:
            if (buttonBack.isPressed()) {
                shutdownWifi();
                if (!initEspNow()) changeMode(MODE_ERROR);
                else changeMode(MODE_HOME_MENU);
                modeHistory.clear();
            }
            break;
        default: break;
    }
}

// [MODIFIED] 8개로 늘어난 메뉴 처리
void handleHomeMenuButtons() {
    if (buttonUp.isPressed()) {
        uiState.menuCursor = (uiState.menuCursor == 0) ? (HOME_MENU_ITEM_COUNT - 1) : (uiState.menuCursor - 1);
    }
    else if (buttonDown.isPressed()) {
        uiState.menuCursor = (uiState.menuCursor == (HOME_MENU_ITEM_COUNT - 1)) ? 0 : (uiState.menuCursor + 1);
    }
    else if (buttonEnter.isPressed()) {
        switch (uiState.menuCursor) {
            case 0:
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_GROUP_EXECUTE_MENU);
                uiState.menuCursor = 0;   // [개조] 그룹 실행 필드 커서 초기화
                adjustingValue = false;
                break;
            case 1:
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_SINGLE_EXECUTE_MENU);
                uiState.singleExecCursor = 0;
                adjustingValue = false;
                break;
            case 2:
                // [AUTO CH] 채널 자동 최적화 모드 진입
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_RF_SCAN);
                break;
            case 3:
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_CLONE_TX);
                break;
            case 4:
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_CLONE_RX);
                break;
            case 5:
                initOtaWorkflow();
                break;
            case 6:
                vibrationEnabled = !vibrationEnabled;
                EEPROM.write(EEPROM_VIB_ENABLED_ADDR, vibrationEnabled ? 0x01 : 0x00);
                EEPROM.commit();
                logPrintf(LogLevel::LOG_INFO, "VIB toggle: %s", vibrationEnabled ? "ON" : "OFF");
                break;
            case 7:
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_LANGUAGE_SETTING);
                uiState.menuCursor = (uint8_t)currentLanguage;
                break;
        }
    }
    updateDisplay();
}

// [NEW] 언어 선택 모드 버튼: UP/DOWN 이동, ENTER 적용(+EEPROM 저장), BACK 나가기.
void handleLanguageMenuButtons() {
    if (buttonUp.isPressed()) {
        uiState.menuCursor = (uiState.menuCursor == 0) ? (LANG_COUNT - 1) : (uiState.menuCursor - 1);
    }
    else if (buttonDown.isPressed()) {
        uiState.menuCursor = (uiState.menuCursor == (LANG_COUNT - 1)) ? 0 : (uiState.menuCursor + 1);
    }
    else if (buttonEnter.isPressed()) {
        setLanguage((Language)uiState.menuCursor); // 즉시 적용 + EEPROM 저장. 화면은 새 언어로 갱신됨.
        logPrintf(LogLevel::LOG_INFO, "Language set: %s", langDisplayName(currentLanguage));
    }
    else if (buttonBack.isPressed()) {
        navigateBack();
    }
    updateDisplay();
}

// [NEW] 복제 송출 (메인 기기) — 송신은 checkExecutionAndMode에서 2초마다 자동 반복.
// 버튼은 나가기(BACK)만 처리.
void handleCloneTxButtons() {
    if (buttonBack.isPressed()) {
        cloneTxSentAt = 0;
        navigateBack();
    }
}

// [NEW] 복제 수신 및 초기화 (스페어 기기)
void handleCloneRxButtons() {
    if (buttonBack.isPressed()) {
        navigateBack();
    } else if (buttonUp.checkHold()) { // 초기화 — 길게 눌러야만 실행 (실수 탭 방지)
        EEPROM.write(EEPROM_CLONED_MAC_FLAG, 0x00);
        EEPROM.commit();
        logPrintf(LogLevel::LOG_INFO, "CLONE: Reset. Rebooting...");
        delay(500);
        ESP.restart();
    }
}

// [NEW] RF 자동 환경 스캔 버튼 제어
// [AUTO CH] 채널 자동 최적화 버튼 처리
void handleRfScanButtons() {
    if (rfScanRunning) {
        // 스캔 진행 중에는 BACK으로 중단만 허용
        if (buttonBack.isPressed()) {
            stopRfScanSequence();
            navigateBack();
        }
        return;
    }
    if (buttonEnter.isPressed()) {
        startRfScanSequence(); // 채널 측정·최적화 시작
    } else if (buttonBack.isPressed()) {
        navigateBack();
    }
}

// ... 아래의 기존 핸들러 함수들은 변경 없이 그대로 이어집니다 ...

// [개조] 커서: 0=PLAY, 1=STYLE, 2=GROUP(편집), 3=MEMBERS(편집화면 진입)
void handleGroupExecuteMenuButtons() {
    uint8_t max_cursor = 3;

    // [홀드] STYLE=HOLD + 커서가 PLAY + 편집중 아님 → ENTER 누르는 동안 그룹 전체 지속 출력
    if (execStyle == STYLE_HOLD && uiState.menuCursor == 0 && !adjustingValue) {
        bool down = buttonEnter.isDown();
        serviceHold(down, /*group=*/true);
        if (down) { updateDisplay(); return; }
    }

    if (adjustingValue) {                       // GROUP 번호 변경
        if (buttonUp.isPressed() || buttonUp.shouldCount()) selectedGroupNum = (selectedGroupNum == 5) ? 1 : selectedGroupNum + 1;
        else if (buttonDown.isPressed() || buttonDown.shouldCount()) selectedGroupNum = (selectedGroupNum == 1) ? 5 : selectedGroupNum - 1;
        if (buttonEnter.isPressed() || buttonBack.isPressed()) adjustingValue = false;
    } else {
        if (buttonUp.isPressed()) uiState.menuCursor = (uiState.menuCursor == 0) ? max_cursor : (uiState.menuCursor - 1);
        else if (buttonDown.isPressed()) uiState.menuCursor = (uiState.menuCursor == max_cursor) ? 0 : (uiState.menuCursor + 1);

        if (buttonEnter.isPressed()) {
            if (uiState.menuCursor == 0) {              // PLAY
                if (execStyle == STYLE_TIMER) { startGroupExecution(micros()); return; }
                // STYLE=HOLD: 홀드 로직(2c)에서.
            } else if (uiState.menuCursor == 1) {       // STYLE 토글
                execStyle = (execStyle == STYLE_TIMER) ? STYLE_HOLD : STYLE_TIMER;
            } else if (uiState.menuCursor == 2) {       // GROUP 번호 편집
                adjustingValue = true;
            } else {                                    // MEMBERS 편집 진입
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_GROUP_MEMBERS);
                uiState.menuCursor = 0;
            }
        }
        if (buttonBack.isPressed()) { navigateBack(); return; }
    }
    updateDisplay();
}

// [NEW] 그룹 멤버 편집: UP/DOWN 이동, ENTER로 현재 그룹 포함/제외 토글(즉시 저장), BACK 복귀
void handleGroupMembersButtons() {
    int total = MAX_DEVICES;
    if (buttonUp.isPressed()) uiState.menuCursor = (uiState.menuCursor == 0) ? (total - 1) : (uiState.menuCursor - 1);
    else if (buttonDown.isPressed()) uiState.menuCursor = (uiState.menuCursor == (total - 1)) ? 0 : (uiState.menuCursor + 1);
    else if (buttonEnter.isPressed()) {
        uint8_t id = uiState.menuCursor + 1;
        deviceSettings[id].groupMembershipBitmask ^= (1 << (selectedGroupNum - 1)); // 토글
        // 해당 장치 설정 즉시 저장 (selectedDevice 전역 안 건드리고 직접 기록)
        uint16_t addr = SETTINGS_START_ADDR + (id - 1) * sizeof(DeviceSettings);
        EEPROM.put(addr, deviceSettings[id]);
        EEPROM.commit();
    }
    else if (buttonBack.isPressed()) { navigateBack(); return; }
    updateDisplay();
}

// [개조] 6행 스크롤: 0=PLAY 1=STYLE 2=ID 3=STEP/TYPE 4=DELAY 5=PLAY (4·5는 정보, 3은 멀티만 편집)
void handleSingleExecuteMenuButtons() {
    DeviceSettings& dev = deviceSettings[selectedDevice];
    const uint8_t max_cursor = 7; // 8행 (PLAY/ID/STYLE/STEP/TYPE/DELAY/PLAY시간/POWER)

    if (execStyle == STYLE_HOLD && uiState.singleExecCursor == 0 && !adjustingValue) {
        bool down = buttonEnter.isDown();
        serviceHold(down, /*group=*/false);
        if (down) { updateDisplay(); return; }
    }

    if (adjustingValue) {
        if (uiState.singleExecCursor == 1) {                // ID
            if (buttonUp.isPressed() || buttonUp.shouldCount()) { selectedDevice = (selectedDevice == MAX_DEVICES) ? 1 : selectedDevice + 1; uiState.selectedStep = 0; }
            else if (buttonDown.isPressed() || buttonDown.shouldCount()) { selectedDevice = (selectedDevice == 1) ? MAX_DEVICES : selectedDevice - 1; uiState.selectedStep = 0; }
        } else if (uiState.singleExecCursor == 3) {         // STEP
            if (uiState.adjustingStepCount) {
                if (buttonUp.isPressed() || buttonUp.shouldCount()) dev.stepCount = (dev.stepCount % MAX_EXECUTION_STEPS) + 1;
                else if (buttonDown.isPressed() || buttonDown.shouldCount()) dev.stepCount = (dev.stepCount == 1) ? MAX_EXECUTION_STEPS : dev.stepCount - 1;
            } else {
                if (buttonUp.isPressed() || buttonUp.shouldCount()) uiState.selectedStep = (uiState.selectedStep + 1) % dev.stepCount;
                else if (buttonDown.isPressed() || buttonDown.shouldCount()) uiState.selectedStep = (uiState.selectedStep == 0) ? (dev.stepCount - 1) : (uiState.selectedStep - 1);
            }
        } else if (uiState.singleExecCursor == 4) {         // TYPE
            if (buttonUp.isPressed() || buttonUp.shouldCount()) dev.machineType = (MachineType)((dev.machineType + 1) % 6);
            else if (buttonDown.isPressed() || buttonDown.shouldCount()) dev.machineType = (dev.machineType == 0) ? MachineType(5) : (MachineType)(dev.machineType - 1);
        }
        if (buttonEnter.isPressed()) {
            if (uiState.singleExecCursor == 3 && !uiState.adjustingStepCount) {
                uiState.adjustingStepCount = true; // 스텝 선택 → ENTER 추가: 총 스텝수 조정
            } else {
                adjustingValue = false;
                uiState.adjustingStepCount = false;
                saveSettings(false);
            }
        } else if (buttonBack.isPressed()) {
            adjustingValue = false;
            uiState.adjustingStepCount = false;
        }
    } else {
        if (buttonUp.isPressed()) uiState.singleExecCursor = (uiState.singleExecCursor == 0) ? max_cursor : (uiState.singleExecCursor - 1);
        else if (buttonDown.isPressed()) uiState.singleExecCursor = (uiState.singleExecCursor == max_cursor) ? 0 : (uiState.singleExecCursor + 1);

        if (buttonEnter.isPressed()) {
            uint8_t c = uiState.singleExecCursor;
            if (c == 0) {                                   // PLAY
                if (execStyle == STYLE_TIMER) { startSingleExecution(selectedDevice, micros()); return; }
            } else if (c == 1) {                            // ID
                adjustingValue = true;
            } else if (c == 2) {                            // STYLE 토글
                execStyle = (execStyle == STYLE_TIMER) ? STYLE_HOLD : STYLE_TIMER;
            } else if (c == 3) {                            // STEP (ENTER 1번=스텝선택, 2번=스텝수 조정)
                adjustingValue = true;
            } else if (c == 4) {                            // TYPE
                adjustingValue = true;
            } else if (c == 5) {                            // DELAY
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_TIME_SETTING_DELAY_EDIT);
                uiState.adjustingUnit = UNIT_MINUTES;
                adjustingValue = false;
            } else if (c == 6) {                            // PLAY time
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_TIME_SETTING_PLAY_EDIT);
                adjustingValue = false;
            } else if (c == 7) {                            // POWER
                modeHistory.push_back(static_cast<Mode>(currentMode));
                changeMode(MODE_TIME_SETTING_PWM_EDIT);
                adjustingValue = false;
            }
        }
        if (buttonBack.isPressed()) { navigateBack(); return; }
    }
    // STEP 수에 따라 deviceType 자동 갱신
    if (dev.stepCount > 1) dev.deviceType = MULTI_ACTION;
    else dev.deviceType = SINGLE_ACTION;
    if (uiState.selectedStep >= dev.stepCount) uiState.selectedStep = dev.stepCount - 1;
    updateDisplay();
}

void handleGroupSettingOverviewButtons() {
    calculateGroupOverviewMetrics();
    const int maxVisibleLines = (DISPLAY_HEIGHT - OLED_MENU_START_Y) / uiLineHeight();
    bool needsUpdate = false;
    if (buttonUp.isPressed() || buttonUp.shouldCount()) {
        bool wrapped = (selectedGroupNum == 1);
        selectedGroupNum = wrapped ? 5 : selectedGroupNum - 1;
        int groupStartLine = groupDisplayLineStarts[selectedGroupNum - 1];
        if (wrapped) uiState.scrollOffset = std::max(0, groupDisplayLineStarts[4] + groupDisplayTotalLines[4] - maxVisibleLines);
        else if (groupStartLine < uiState.scrollOffset) uiState.scrollOffset = groupStartLine;
        needsUpdate = true;
    } else if (buttonDown.isPressed() || buttonDown.shouldCount()) {
        bool wrapped = (selectedGroupNum == 5);
        selectedGroupNum = wrapped ? 1 : selectedGroupNum + 1;
        int groupEndLine = groupDisplayLineStarts[selectedGroupNum - 1] + groupDisplayTotalLines[selectedGroupNum - 1];
        if (wrapped) uiState.scrollOffset = 0;
        else if (groupEndLine > (uiState.scrollOffset + maxVisibleLines)) uiState.scrollOffset = groupEndLine - maxVisibleLines;
        needsUpdate = true;
    } else if (buttonEnter.isPressed()) {
        modeHistory.push_back(static_cast<Mode>(currentMode));
        changeMode(MODE_GROUP_SETTING_DETAIL);
        selectedDevice = 1;
        needsUpdate = true;
    } else if (buttonBack.isPressed()) {
        navigateBack();
        return;
    }
    if (needsUpdate) updateDisplay();
}

void handleGroupSettingDetailButtons() {
    if (buttonUp.isPressed() || buttonUp.shouldCount()) selectedDevice = (selectedDevice == MAX_DEVICES) ? 1 : selectedDevice + 1;
    else if (buttonDown.isPressed() || buttonDown.shouldCount()) selectedDevice = (selectedDevice == 1) ? MAX_DEVICES : selectedDevice - 1;
    else if (buttonEnter.isPressed()) {
        deviceSettings[selectedDevice].groupMembershipBitmask ^= (1 << (selectedGroupNum - 1));
        saveSettings(true);
    } else if (buttonBack.isPressed()) navigateBack();
    updateDisplay();
}

void handleTimeSettingOverviewButtons() {
    auto& settings = deviceSettings[selectedDevice];
    const int maxCursor = 5; 

    if (adjustingValue) {
        OledMenuState currentField = (OledMenuState)uiState.timeSetCursor;
        if (currentField == FIELD_ID) {
            if (buttonUp.isPressed() || buttonUp.shouldCount()) selectedDevice = (selectedDevice % MAX_DEVICES) + 1;
            else if (buttonDown.isPressed() || buttonDown.shouldCount()) selectedDevice = (selectedDevice == 1) ? MAX_DEVICES : selectedDevice - 1;
        } 
        else if (currentField == FIELD_TYPE) { 
            if (buttonUp.isPressed() || buttonUp.shouldCount()) settings.machineType = (MachineType)((settings.machineType + 1) % 6);
            else if (buttonDown.isPressed() || buttonDown.shouldCount()) settings.machineType = (settings.machineType == 0) ? (MachineType)(5) : (MachineType)(settings.machineType - 1);
        }
        else if (currentField == FIELD_STEP) {
             if (uiState.adjustingStepCount) { 
                if (buttonUp.isPressed() || buttonUp.shouldCount()) settings.stepCount = (settings.stepCount % MAX_EXECUTION_STEPS) + 1;
                else if (buttonDown.isPressed() || buttonDown.shouldCount()) settings.stepCount = (settings.stepCount == 1) ? MAX_EXECUTION_STEPS : settings.stepCount - 1;
            } else { 
                if (buttonUp.isPressed() || buttonUp.shouldCount()) uiState.selectedStep = (uiState.selectedStep + 1) % settings.stepCount;
                else if (buttonDown.isPressed() || buttonDown.shouldCount()) uiState.selectedStep = (uiState.selectedStep == 0) ? settings.stepCount - 1 : uiState.selectedStep - 1;
            }
        }

        if (buttonEnter.isPressed()) {
             if (currentField == FIELD_STEP && !uiState.adjustingStepCount) uiState.adjustingStepCount = true; 
             else {
                adjustingValue = false;
                uiState.adjustingStepCount = false;
                saveSettings(false);
             }
        } else if (buttonBack.isPressed()) {
            adjustingValue = false;
            uiState.adjustingStepCount = false;
        }

    } else { 
        if (buttonUp.isPressed()) uiState.timeSetCursor = (uiState.timeSetCursor == 0) ? maxCursor : uiState.timeSetCursor - 1;
        else if (buttonDown.isPressed()) uiState.timeSetCursor = (uiState.timeSetCursor == maxCursor) ? 0 : uiState.timeSetCursor + 1;

        if (buttonEnter.isPressed()) {
            OledMenuState currentField = (OledMenuState)uiState.timeSetCursor;
            if (currentField == FIELD_ID || currentField == FIELD_STEP || currentField == FIELD_TYPE) adjustingValue = true;
            else if (currentField == FIELD_DELAY) { modeHistory.push_back(static_cast<Mode>(currentMode)); changeMode(MODE_TIME_SETTING_DELAY_EDIT); }
            else if (currentField == FIELD_PLAY) { modeHistory.push_back(static_cast<Mode>(currentMode)); changeMode(MODE_TIME_SETTING_PLAY_EDIT); }
            else if (currentField == FIELD_PWM) { modeHistory.push_back(static_cast<Mode>(currentMode)); changeMode(MODE_TIME_SETTING_PWM_EDIT); }
        } else if (buttonBack.isPressed()) { navigateBack(); return; }
    }
    
    if (settings.stepCount > 1) settings.deviceType = MULTI_ACTION;
    else settings.deviceType = SINGLE_ACTION;
    if (uiState.selectedStep >= settings.stepCount) uiState.selectedStep = settings.stepCount - 1;
    updateDisplay();
}

void handleTimeSettingDelayEditButtons() {
    if (buttonUp.isPressed() || (adjustingValue && buttonUp.shouldCount())) {
        if (adjustingValue) increaseTimerValue();
        else uiState.adjustingUnit = (uiState.adjustingUnit == UNIT_MINUTES) ? UNIT_SECONDS : UNIT_MINUTES;
    } else if (buttonDown.isPressed() || (adjustingValue && buttonDown.shouldCount())) {
        if (adjustingValue) decreaseTimerValue();
        else uiState.adjustingUnit = (uiState.adjustingUnit == UNIT_MINUTES) ? UNIT_SECONDS : UNIT_MINUTES;
    } else if (buttonEnter.isPressed()) {
        adjustingValue = !adjustingValue;
        if (!adjustingValue) saveSettings(false);
    } else if (buttonBack.isPressed()) {
        if (adjustingValue) adjustingValue = false;
        else navigateBack();
    }
    updateDisplay();
}

void handleTimeSettingPlayEditButtons() {
    if (buttonUp.isPressed() || (adjustingValue && buttonUp.shouldCount())) {
        if (!adjustingValue) adjustingValue = true;
        increaseTimerValue();
    } else if (buttonDown.isPressed() || (adjustingValue && buttonDown.shouldCount())) {
        if (!adjustingValue) adjustingValue = true;
        decreaseTimerValue();
    } else if (buttonEnter.isPressed()) {
        adjustingValue = !adjustingValue;
        if (!adjustingValue) saveSettings(false);
    } else if (buttonBack.isPressed()) {
        if (adjustingValue) adjustingValue = false;
        else navigateBack();
    }
    updateDisplay();
}

void handleTimeSettingPwmEditButtons() {
    if (buttonUp.isPressed() || (adjustingValue && buttonUp.shouldCount())) {
        if (!adjustingValue) adjustingValue = true;
        increaseTimerValue();
    } else if (buttonDown.isPressed() || (adjustingValue && buttonDown.shouldCount())) {
        if (!adjustingValue) adjustingValue = true;
        decreaseTimerValue();
    } else if (buttonEnter.isPressed()) {
        adjustingValue = !adjustingValue;
        if (!adjustingValue) saveSettings(false);
    } else if (buttonBack.isPressed()) {
        if (adjustingValue) adjustingValue = false;
        else navigateBack();
    }
    updateDisplay();
}

void handleUpdatePageButtons() {
    if (buttonUp.isPressed()) { if (otaState.scrollOffset > 0) otaState.scrollOffset--; }
    else if (buttonDown.isPressed()) { otaState.scrollOffset++; }
    else if (buttonEnter.isPressed()) {
        if (otaState.updateAvailable && !otaState.inProgress) { modeHistory.push_back(static_cast<Mode>(currentMode)); changeMode(MODE_OTA_CONFIRM); }
    } else if (buttonBack.isPressed()) {
        // WiFi를 유지한 채 AP 화면으로 복귀 — AP 화면에서 Back을 눌러야 WiFi 종료
        if (!otaState.inProgress) changeMode(MODE_OTA_WIFI_AP);
    }
    updateDisplay();
}

void handleOtaConfirmButtons() {
    if (buttonEnter.isPressed()) { otaState.updateConfirmed = true; changeMode(MODE_OTA_DOWNLOADING); }
    else if (buttonBack.isPressed()) navigateBack();
    updateDisplay();
}

void handleExecutionModeButtons() {
    if (buttonBack.checkHold() && (millis() - modeEnteredAt >= BUTTON_HOLD_TIME)) {
        // B 길게: 모든 수신기에 취소 명령 전송 후 취소 메시지 표시
        sendCancelCommand(0);
        isProcessing = false;
        groupDeviceCount = 0;
        executionCompleteTime = millis();
        changeMode(MODE_CANCEL_MESSAGE);
    } else if (buttonEnter.isPressed()) {
        // ENTER: 수신기 실행은 그대로 두고 송신기만 홈으로 복귀
        isProcessing = false;
        groupDeviceCount = 0;
        changeMode(MODE_HOME_MENU);
        modeHistory.clear();
        uiState.menuCursor = 0;
    }
}

void handleTimerMonitorButtons() {
    if (buttonBack.checkHold() && (millis() - modeEnteredAt >= BUTTON_HOLD_TIME)) {
        sendCancelCommand(0);
        isProcessing = false;
        groupDeviceCount = 0;
        executionCompleteTime = millis();
        changeMode(MODE_CANCEL_MESSAGE);
        return;
    }
    if (buttonEnter.isPressed()) {
        isProcessing = false;
        groupDeviceCount = 0;
        changeMode(MODE_HOME_MENU);
        modeHistory.clear();
        uiState.menuCursor = 0;
        return;
    }
    if (buttonUp.isPressed()) { if (uiState.scrollOffset > 0) uiState.scrollOffset--; updateDisplay(); }
    else if (buttonDown.isPressed()) { uiState.scrollOffset++; updateDisplay(); }
}

void handleCompletionModeButtons() {
    if (buttonUp.isPressed() || buttonDown.isPressed() || buttonBack.isPressed() || buttonEnter.isPressed()) {
        isProcessing = false;
        executionComplete = true;
        groupDeviceCount = 0;
        uiState.scrollOffset = 0;
        if (previousSelectedDevice == 0) changeMode(MODE_GROUP_EXECUTE_MENU);
        else changeMode(MODE_SINGLE_EXECUTE_MENU);
    }
}

void handleCancelMessageButtons() {
    if (buttonUp.isPressed() || buttonDown.isPressed() || buttonBack.isPressed() || buttonEnter.isPressed()) {
        isProcessing = false;
        groupDeviceCount = 0;
        uiState.scrollOffset = 0;
        changeMode(MODE_HOME_MENU);
        modeHistory.clear();
    }
}

void handleApModeButtons() {
    if (buttonBack.isPressed()) navigateBack();
}

void handleOtaErrorButtons() {
    if (buttonUp.isPressed() || buttonDown.isPressed() || buttonEnter.isPressed() || buttonBack.isPressed()) {
        // WiFi를 종료하지 않고 AP 화면으로 복귀 → 사용자가 수동으로 Back을 누를 때까지 WiFi 유지
        changeMode(MODE_OTA_WIFI_AP);
    }
}

void handleOtaSuccessButtons() {
    if (buttonUp.isPressed() || buttonDown.isPressed() || buttonEnter.isPressed() || buttonBack.isPressed()) {
        logPrintf(LogLevel::LOG_INFO, "[BUTTON] OTA Success acknowledged, rebooting...");
        Serial.end();
        delay(200);
        ESP.restart();
    }
}