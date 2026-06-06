// hardware_buttons.h
#ifndef HARDWARE_BUTTONS_H
#define HARDWARE_BUTTONS_H

#include <Arduino.h>
#include "config_t.h"
#include "utils_t.h"

class Button {
private:
    uint8_t pin;
    bool pressed;
    bool state;
    bool lastState;
    unsigned long lastDebounceTime;
    unsigned long lastPressTime;
    unsigned long debounceDelay;
    unsigned long lastActionTime;
    bool isHolding;
    unsigned long countInterval;
    uint8_t pressCount;
public:
    Button(uint8_t p);
    void begin();
    void update();
    bool isPressed();
    bool checkHold();
    bool shouldCount();
    void resetPressCount();
};

extern Button buttonUp, buttonDown, buttonBack, buttonEnter;

void handleButtons();
void handleHomeMenuButtons();
void handleGroupExecuteMenuButtons();
void handleSingleExecuteMenuButtons();
void handleGroupSettingOverviewButtons();
void handleGroupSettingDetailButtons();
void handleTimeSettingOverviewButtons();
void handleDeviceTypeSettingButtons();
void handleMultiActionOverviewButtons();
void handleMultiActionDetailButtons();
void handleTimeSettingDelayEditButtons();
void handleTimeSettingPlayEditButtons();
void handleTimeSettingPwmEditButtons(); 
void handleUpdatePageButtons();
void handleOtaConfirmButtons();
void handleExecutionModeButtons();
void handleTimerMonitorButtons();       
void handleCompletionModeButtons();
void handleApModeButtons();
void handleOtaErrorButtons();

// [NEW] 복제 모드용 버튼 핸들러
void handleCloneTxButtons();
void handleCloneRxButtons();

#endif // HARDWARE_BUTTONS_H