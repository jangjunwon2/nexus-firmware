// transmitter_seeed/hardware_init.cpp

#include "hardware_init.h"
#include "hardware_buttons.h"
#include "hardware_display.h"
#include "driver/rtc_io.h"
#include "hardware_core.h"
#include "esp_sleep.h"
#include "ui_text_t.h"   // U8g2 다국어 텍스트 레이어

Adafruit_SSD1306 display(DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, -1);
Button buttonUp(BUTTON_UP_PIN), buttonDown(BUTTON_DOWN_PIN), buttonBack(BUTTON_BACK_PIN), buttonEnter(BUTTON_ENTER_PIN);

// ... (initDisplay, initVibrationMotor, initBatteryMonitor 함수는 변경 없음) ...
bool initDisplay() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);
    int retryCount = 0;
    while (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS) && retryCount < 3) {
        delay(100);
        retryCount++;
    }
    if (retryCount >= 3) {
        logPrintf(LogLevel::LOG_ERROR, "HW: OLED final initialization failed!");
        return false;
    }
    display.clearDisplay();
    uiTextBegin();   // [다국어] U8g2 텍스트 레이어 초기화 (display.begin 이후)
    oledInitialized = true;
    return true;
}
void initVibrationMotor() {
    pinMode(VIB_MOTOR_PIN, OUTPUT);
    digitalWrite(VIB_MOTOR_PIN, LOW);
    logPrintf(LogLevel::LOG_INFO, "HW: Vibration motor initialized.");
}
void initBatteryMonitor() {
    pinMode(BATTERY_ADC_PIN, INPUT);
    logPrintf(LogLevel::LOG_INFO, "HW: Battery monitoring initialized.");
}


bool initHardware() {
    logPrintf(LogLevel::LOG_INFO, "Booting normally.");

    rgbLedWrite(RGB_LED_PIN, 0, 0, 0);

    bool displayOK = initDisplay();

    buttonUp.begin();
    buttonDown.begin();
    buttonBack.begin();
    buttonEnter.begin();

    initVibrationMotor();
    // initEEPROM()은 setup()에서 이미 호출됨 (MAC 복제·채널·언어 로드가 선행 필요)
    loadSettings();
    initBatteryMonitor();

    batteryVoltage = readBatteryVoltage();
    batteryPercentage = getBatteryPercentage(batteryVoltage);

    currentMode = MODE_HOME_MENU;
    selectedDevice = 1;
    uiState.menuCursor = 0;
    uiState.scrollOffset = 0;

    logPrintf(LogLevel::LOG_INFO, "HW: Hardware initialization complete.");
    return displayOK;
}