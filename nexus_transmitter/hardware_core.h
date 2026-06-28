#ifndef HARDWARE_CORE_H
#define HARDWARE_CORE_H

#include <Arduino.h>
#include "config_t.h"       // For Mode, RunningDevice, DeviceSettings, global variables
#include "utils_t.h"        // For logPrintf, saveSettings, getTimerMs
#include "espnow_t.h"       // For manageCommunication, sendFireBurst
#include "hardware_buttons.h" // For Button objects used in updateButtons()

//────────────────────────────────────────────────────────────────────────
// Hardware Core Update Functions
//────────────────────────────────────────────────────────────────────────
void updateButtons(); // Calls update() of Button objects
void updateVibrationMotor();
void updateBatteryLevel();
void checkExecutionAndMode(); // Main execution logic and mode switching

//────────────────────────────────────────────────────────────────────────
// Other Hardware Control Functions
//────────────────────────────────────────────────────────────────────────
void increaseTimerValue(); // Value adjustment functions
void decreaseTimerValue(); // Value adjustment functions
void startMotorVibration(uint16_t duration, bool isError = false);
void processBackgroundTimer(); // [NEW] 백그라운드 타이머 로직

//────────────────────────────────────────────────────────────────────────
// Battery Related Functions
//────────────────────────────────────────────────────────────────────────
float readBatteryVoltage();
int getBatteryPercentage(float voltage);

//────────────────────────────────────────────────────────────────────────
// Execution Logic Functions
//────────────────────────────────────────────────────────────────────────
void prepareForExecution();
void startGroupExecution(unsigned long buttonPressTime);
void startSingleExecution(uint8_t deviceID, unsigned long buttonPressTime);
void serviceHold(bool down, bool group); // [NEW] 홀드 킵얼라이브 서비스 (매 루프 호출)

#endif // HARDWARE_CORE_H