// transmitter/hardware_buttons.cpp

#include "config_t.h"
#include "hardware_buttons.h"
#include "hardware_core.h"
#include "hardware_display.h"
#include "hardware_init.h"
#include <algorithm>
#include "ota_manager.h"
#include "espnow_t.h"
#include "driver/rtc_io.h"
#include "utils_t.h"

void changeMode(Mode newMode) {
    if (currentMode != newMode) {
        logPrintf(LogLevel::LOG_INFO, "[STATE] Mode Changed: %s -> %s", getModeString(currentMode), getModeString(newMode));
        currentMode = newMode;
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

        bool wasInOtaMode = (previousMode >= MODE_UPDATE_PAGE && previousMode <= MODE_OTA_ERROR);
        bool isStillInOtaMode = (targetMode >= MODE_UPDATE_PAGE && targetMode <= MODE_OTA_ERROR);

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

    updateButtons();
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
        
        // [NEW] 복제 모드 연결
        case MODE_CLONE_TX:             handleCloneTxButtons(); break;
        case MODE_CLONE_RX:             handleCloneRxButtons(); break;
        
        case MODE_BOOT:
        case MODE_OTA_SCANNING:
        case MODE_OTA_DOWNLOADING:
        case MODE_OTA_UPDATING:
        case MODE_OTA_SUCCESS:
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

// [MODIFIED] 7개로 늘어난 메뉴 처리
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
                modeHistory.push_back(currentMode);
                changeMode(MODE_GROUP_EXECUTE_MENU);
                break;
            case 1:
                modeHistory.push_back(currentMode);
                changeMode(MODE_SINGLE_EXECUTE_MENU);
                uiState.singleExecCursor = 0;
                adjustingValue = false;
                break;
            case 2:
                modeHistory.push_back(currentMode);
                changeMode(MODE_GROUP_SETTING_OVERVIEW);
                uiState.scrollOffset = 0;
                break;
            case 3:
                modeHistory.push_back(currentMode);
                changeMode(MODE_TIME_SETTING_OVERVIEW);
                uiState.timeSetCursor = 0;
                adjustingValue = false;
                selectedDevice = 1;
                break;
            case 4:
                initOtaWorkflow();
                break;
            // [NEW] 복제 모드 진입
            case 5:
                modeHistory.push_back(currentMode);
                changeMode(MODE_CLONE_TX);
                break;
            case 6:
                modeHistory.push_back(currentMode);
                changeMode(MODE_CLONE_RX);
                break;
        }
    }
    updateDisplay();
}

// [NEW] 복제 송출 (메인 기기)
void handleCloneTxButtons() {
    if (buttonEnter.isPressed()) {
        sendCloneAnnounce();
    } else if (buttonBack.isPressed()) {
        navigateBack();
    }
}

// [NEW] 복제 수신 및 초기화 (스페어 기기)
void handleCloneRxButtons() {
    if (buttonBack.isPressed()) {
        navigateBack();
    } else if (buttonUp.isPressed() || buttonUp.checkHold()) { // 초기화 (독립 기기로 복귀)
        EEPROM.write(EEPROM_CLONED_MAC_FLAG, 0x00);
        EEPROM.commit();
        logPrintf(LogLevel::LOG_INFO, "CLONE: Reset. Rebooting...");
        delay(500);
        ESP.restart();
    }
}

// ... 아래의 기존 핸들러 함수들은 변경 없이 그대로 이어집니다 ...

void handleGroupExecuteMenuButtons() {
    if (buttonUp.isPressed() || buttonUp.shouldCount()) selectedGroupNum = (selectedGroupNum == 1) ? 5 : selectedGroupNum - 1;
    else if (buttonDown.isPressed() || buttonDown.shouldCount()) selectedGroupNum = (selectedGroupNum == 5) ? 1 : selectedGroupNum + 1;
    else if (buttonEnter.isPressed()) startGroupExecution(micros());
    else if (buttonBack.isPressed()) navigateBack();
    updateDisplay();
}

void handleSingleExecuteMenuButtons() {
    DeviceSettings& current_device = deviceSettings[selectedDevice];
    uint8_t max_cursor = (current_device.deviceType == MULTI_ACTION && current_device.stepCount > 1) ? 2 : 1;

    if (adjustingValue) {
        if (uiState.singleExecCursor == 1) { 
            if (buttonUp.isPressed() || buttonUp.shouldCount()) {
                selectedDevice = (selectedDevice == MAX_DEVICES) ? 1 : selectedDevice + 1;
                uiState.selectedStep = 0;
            } else if (buttonDown.isPressed() || buttonDown.shouldCount()) {
                selectedDevice = (selectedDevice == 1) ? MAX_DEVICES : selectedDevice - 1;
                uiState.selectedStep = 0;
            }
        } else if (uiState.singleExecCursor == 2) { 
             if (buttonUp.isPressed() || buttonUp.shouldCount()) uiState.selectedStep = (uiState.selectedStep + 1) % current_device.stepCount;
             else if (buttonDown.isPressed() || buttonDown.shouldCount()) uiState.selectedStep = (uiState.selectedStep == 0) ? (current_device.stepCount - 1) : (uiState.selectedStep - 1);
        }
        if (buttonEnter.isPressed() || buttonBack.isPressed()) adjustingValue = false;
    } else {
        if (buttonUp.isPressed()) uiState.singleExecCursor = (uiState.singleExecCursor == 0) ? max_cursor : (uiState.singleExecCursor - 1);
        else if (buttonDown.isPressed()) uiState.singleExecCursor = (uiState.singleExecCursor == max_cursor) ? 0 : (uiState.singleExecCursor + 1);

        if (buttonEnter.isPressed()) {
            if (uiState.singleExecCursor == 0) {
                startSingleExecution(selectedDevice, micros());
                return;
            } else if (uiState.singleExecCursor == 1 || uiState.singleExecCursor == 2) adjustingValue = true;
        }
        if (buttonBack.isPressed()) { navigateBack(); return; }
    }
    updateDisplay();
}

void handleGroupSettingOverviewButtons() {
    calculateGroupOverviewMetrics();
    const int maxVisibleLines = (DISPLAY_HEIGHT - OLED_MENU_START_Y) / OLED_LINE_HEIGHT;
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
        modeHistory.push_back(currentMode);
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
            else if (currentField == FIELD_DELAY) { modeHistory.push_back(currentMode); changeMode(MODE_TIME_SETTING_DELAY_EDIT); }
            else if (currentField == FIELD_PLAY) { modeHistory.push_back(currentMode); changeMode(MODE_TIME_SETTING_PLAY_EDIT); }
            else if (currentField == FIELD_PWM) { modeHistory.push_back(currentMode); changeMode(MODE_TIME_SETTING_PWM_EDIT); }
        } else if (buttonBack.isPressed()) { navigateBack(); return; }
    }
    
    if (settings.stepCount > 1) settings.deviceType = MULTI_ACTION;
    else settings.deviceType = SINGLE_ACTION;
    if (uiState.selectedStep >= settings.stepCount) uiState.selectedStep = settings.stepCount - 1;
    updateDisplay();
}

void handleTimeSettingDelayEditButtons() {
    auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
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
    auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
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
    auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
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
        if (otaState.updateAvailable && !otaState.inProgress) { modeHistory.push_back(currentMode); changeMode(MODE_OTA_CONFIRM); }
    } else if (buttonBack.isPressed()) {
        if (!otaState.inProgress) navigateBack();
    }
    updateDisplay();
}

void handleOtaConfirmButtons() {
    if (buttonEnter.isPressed()) { otaState.updateConfirmed = true; changeMode(MODE_OTA_DOWNLOADING); }
    else if (buttonBack.isPressed()) navigateBack();
    updateDisplay();
}

void handleExecutionModeButtons() {
    if (buttonBack.checkHold()) {
        isProcessing = false;
        groupDeviceCount = 0;
        changeMode(MODE_HOME_MENU);
        modeHistory.clear();
        uiState.menuCursor = 0;
    }
}

void handleTimerMonitorButtons() {
    if (buttonEnter.isPressed()) {
        if (previousSelectedDevice == 0) changeMode(MODE_GROUP_EXECUTE_MENU);
        else changeMode(MODE_SINGLE_EXECUTE_MENU);
        modeHistory.clear();
        modeHistory.push_back(MODE_HOME_MENU);
        return;
    }
    if (buttonBack.checkHold()) {
        isProcessing = false; 
        groupDeviceCount = 0;
        changeMode(MODE_HOME_MENU);
        modeHistory.clear();
        uiState.menuCursor = 0;
    }
    if (buttonUp.isPressed()) { if (uiState.scrollOffset > 0) uiState.scrollOffset--; updateDisplay(); }
    else if (buttonDown.isPressed()) { uiState.scrollOffset++; updateDisplay(); }
}

void handleCompletionModeButtons() {
    if (buttonUp.isPressed() || buttonDown.isPressed() || buttonBack.isPressed() || buttonEnter.isPressed()) {
        if (previousSelectedDevice == 0) changeMode(MODE_GROUP_EXECUTE_MENU);
        else changeMode(MODE_SINGLE_EXECUTE_MENU);
        isProcessing = false;
        executionComplete = true;
        groupDeviceCount = 0;
        uiState.scrollOffset = 0;
    }
}

void handleApModeButtons() {
    if (buttonBack.isPressed()) navigateBack();
}

void handleOtaErrorButtons() {
    if (buttonUp.isPressed() || buttonDown.isPressed() || buttonEnter.isPressed() || buttonBack.isPressed()) {
        shutdownWifi();
        if (!initEspNow()) changeMode(MODE_ERROR);
        else changeMode(MODE_HOME_MENU);
        uiState.menuCursor = 0;
        modeHistory.clear();
    }
}