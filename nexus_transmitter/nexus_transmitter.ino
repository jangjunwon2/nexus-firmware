// transmitter/main.ino

#include "config_t.h"
#include "hardware_init.h"
#include "hardware_display.h"
#include "hardware_buttons.h"
#include "hardware_core.h"
#include "utils_t.h"
#include "espnow_t.h"
#include "ota_manager.h"
#include <esp_mac.h> // [NEW] MAC 주소 변경 함수 사용

void setup() {
    initLog();
    
    // [NEW] Wi-Fi 및 하드웨어 초기화 전에 MAC 주소 복제 검사
    EEPROM.begin(EEPROM_SIZE);
    if (EEPROM.read(EEPROM_CLONED_MAC_FLAG) == 0xAA) {
        uint8_t clonedMac[6];
        for (int i = 0; i < 6; i++) {
            clonedMac[i] = EEPROM.read(EEPROM_CLONED_MAC_ADDR + i);
        }
        esp_base_mac_addr_set(clonedMac); // MAC 주소 강제 덮어쓰기!
        logPrintf(LogLevel::LOG_INFO, "SYSTEM: Cloned MAC Applied! %02X:%02X:%02X:%02X:%02X:%02X", 
                  clonedMac[0], clonedMac[1], clonedMac[2], clonedMac[3], clonedMac[4], clonedMac[5]);
    } else {
        logPrintf(LogLevel::LOG_INFO, "SYSTEM: Original MAC Mode");
    }

    Serial.println("\n\n[INFO] SYSTEM: Transmitter Device Booting...");
    logPrintf(LogLevel::LOG_INFO, "Current Firmware Version: %s", firmwareVersion.c_str());
    
    if (!initHardware()) {
        logPrintf(LogLevel::LOG_ERROR, "SYSTEM: Hardware Initialization Failed! Entering Error Mode.");
        currentMode = MODE_ERROR;
        updateDisplay();
        while (true) { delay(1000); }
    }

    if (!initEspNow()) {
        logPrintf(LogLevel::LOG_ERROR, "SYSTEM: ESP-NOW Initialization Failed! Entering Error Mode.");
        currentMode = MODE_ERROR;
        updateDisplay();
        while (true) { delay(1000); }
    }

    logPrintf(LogLevel::LOG_INFO, "SYSTEM: Transmitter Device Ready.");
    updateDisplay();
}

void loop() {
    updateButtons();
    handleButtons();
    updateVibrationMotor();
    updateDisplay();
    updateBatteryLevel();
    
    if (!isOtaMode(currentMode)) {
        checkExecutionAndMode();
        manageCommunication();
    }
    
    delay(10); // 과도한 CPU 점유 방지용 짧은 딜레이
}