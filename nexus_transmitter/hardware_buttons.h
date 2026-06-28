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
    bool isDown() const { return state == LOW; } // [NEW] 현재 눌려 있는지(홀드 감지용)
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
void handleCancelMessageButtons();
void handleApModeButtons();
void handleOtaErrorButtons();
void handleOtaSuccessButtons();

// [NEW] 복제 모드용 버튼 핸들러
void handleCloneTxButtons();
void handleCloneRxButtons();

// [NEW] RF 자동 환경 스캔 모드 버튼 핸들러
void handleRfScanButtons();

// [NEW] 언어 선택 모드 버튼 핸들러
void handleLanguageMenuButtons();

// [NEW] 그룹 멤버 편집 모드 버튼 핸들러
void handleGroupMembersButtons();

void changeMode(Mode newMode);

// SYNC(MAIN) 전송 시각 (0 = 미전송, 화면 피드백용)
extern unsigned long cloneTxSentAt;

#endif // HARDWARE_BUTTONS_H