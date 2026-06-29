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
    
    // MAC 복제·채널·언어 설정은 initHardware()/initEspNow() 전에 읽어야 하므로 여기서 먼저 초기화.
    // initHardware() 내부의 중복 호출은 제거됨.
    initEEPROM();
    
    // [AUTO CH] 저장된 최적 채널 로드 (AUTO CH 스캔으로 결정·저장됨, 기본/검증실패 시 Ch 1).
    // 페어링·AUTO CH 핸드셰이크는 항상 Ch 1을 랑데부로 사용하므로, 채널이 어긋나도
    // AUTO CH 한 번이면 재동기화됨(자가 복구).
    uint8_t savedChan = EEPROM.read(EEPROM_WIFI_CHANNEL_ADDR);
    WIFI_CHANNEL = (savedChan == 1 || savedChan == 6 || savedChan == 11) ? savedChan : 1;

    // [다국어] 저장된 OLED 표시 언어 로드 (미기록 시 영어)
    loadLanguage();

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

    logPrintf(LogLevel::LOG_INFO, "SYSTEM: Transmitter Device Booting...");
    logPrintf(LogLevel::LOG_INFO, "Firmware v%s | %s", firmwareVersion.c_str(), firmwareNotes.c_str());
    
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
    // [FIX] CLONE RX: Wi-Fi 콜백 밖에서 안전하게 EEPROM 저장 및 재부팅 처리
    if (cloneReceivedFlag) {
        cloneReceivedFlag = false;
        // MAC 저장
        for (int i = 0; i < 6; i++) EEPROM.write(EEPROM_CLONED_MAC_ADDR + i, clonedMacBuffer[i]);
        EEPROM.write(EEPROM_CLONED_MAC_FLAG, 0xAA);
        // [NEW] AUTO CH 채널 저장
        EEPROM.write(EEPROM_WIFI_CHANNEL_ADDR, cloneRxChannelBuffer);
        // [NEW] 장치 설정 20개 저장
        for (uint8_t id = 1; id <= MAX_DEVICES; id++) {
            uint16_t addr = SETTINGS_START_ADDR + (id - 1) * sizeof(DeviceSettings);
            EEPROM.put(addr, cloneRxSettingsBuffer[id]);
        }
        EEPROM.commit();
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        displayCenteredModeName("SPARE COPY");
        display.setCursor(TEXT_X, OLED_MENU_START_Y + 9);
        display.println("COPY SUCCESS!");
        display.setCursor(TEXT_X, OLED_MENU_START_Y + 18);
        display.println("Rebooting...");
        display.display();
        logPrintf(LogLevel::LOG_INFO, "CLONE: SUCCESS! MAC+Ch%d+Settings saved. Rebooting...", cloneRxChannelBuffer);
        delay(500);
        ESP.restart();
    }

    // [다국어] 웹 UI 등에서 변경된 언어 설정을 안전하게 EEPROM에 커밋
    if (pendingEepromCommit) {
        pendingEepromCommit = false;
        EEPROM.commit();
        logPrintf(LogLevel::LOG_INFO, "EEPROM: Committed language changes safely from loop.");
    }

    updateButtons();   // 버튼 상태 읽기 (handleButtons 내부 중복 호출 제거됨)
    handleButtons();
    updateVibrationMotor();
    updateDisplay();
    updateBatteryLevel();
    
    if (!isOtaMode(currentMode)) {
        checkExecutionAndMode();
    }
    
    delay(10); // 과도한 CPU 점유 방지용 짧은 딜레이
}