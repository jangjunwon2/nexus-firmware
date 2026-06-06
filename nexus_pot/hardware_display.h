// hardware_display.h
#ifndef HARDWARE_DISPLAY_H
#define HARDWARE_DISPLAY_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <stdarg.h> 
#include "config_t.h"

extern Adafruit_SSD1306 display;

extern int groupDisplayLineStarts[5];
extern int groupDisplayTotalLines[5];

void updateDisplay(); 
void displayBootScreen();
void displayHomeMenu();
void displayGroupExecuteMenu();
void displaySingleExecuteMenu();
void displayGroupSettingOverview();
void displayGroupSettingDetail();
void displayTimeSettingOverview();
void displayDeviceTypeSetting();     
void displayMultiActionOverview();   
void displayMultiActionDetail();     
void displayTimeSettingDelayEdit();
void displayTimeSettingPlayEdit();
void displayTimeSettingPwmEdit();    
void displayUpdatePage();
void displayExecutionMode();
void displayTimerMonitor();          
void displayCompletionMessage();
void displayCenteredModeName(const char* modeName);
void displayBatteryIcon();
void drawBlinkingText(int x, int y, const char* format, ...);

void calculateGroupOverviewMetrics();

void displayOtaWifiAp();
void displayOtaConnecting();
void displayOtaChecking();
void displayOtaConfirm();
void displayOtaScanning();
void displayOtaDownloading();
void displayOtaError();

// [NEW] 복제 모드 UI
void displayCloneTxMode();
void displayCloneRxMode();

const char* getMachineTypeStr(MachineType type);

#endif // HARDWARE_DISPLAY_H