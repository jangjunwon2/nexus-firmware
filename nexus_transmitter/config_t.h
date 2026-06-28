// transmitter_s3/config_t.h

#ifndef CONFIG_T_H
#define CONFIG_T_H

// ── 펌웨어 버전 ──────────────────────────────────────────────────────────────
constexpr const char* FIRMWARE_VERSION = "1.1";
constexpr const char* FIRMWARE_NOTES   = "Initial release";
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <vector>
#include "espnow_comm_shared.h"
#include "i18n_t.h"   // 다국어 문자열 테이블 t(StrId)

#define DEBUG_MODE 0 

#define VIB_MOTOR_STRENGTH    255 
#define RESULT_MESSAGE_DURATION_MS 1000

#define VIB_MOTOR_PIN         5  
#define I2C_SDA_PIN           8   
#define I2C_SCL_PIN           9   
#define OLED_ADDRESS          0x3C
#define BUTTON_UP_PIN         6
#define BUTTON_DOWN_PIN       2   
#define BUTTON_BACK_PIN       4   
#define BUTTON_ENTER_PIN      10  
#define BATTERY_ADC_PIN       1   

#define MAX_DEVICES           20
#define MAX_GROUP_DEVICES     20
#define HOME_MENU_ITEM_COUNT  8 // TIME SET 통폐합(단일 실행에 흡수) = 8개
#define EEPROM_SIZE           4096 
#define MAX_DELAY_MINUTES     59
#define MAX_DELAY_SECONDS     59
#define MIN_PLAY_SECONDS      1
#define MAX_PLAY_SECONDS      60

// 스모크 머신 안전 제한 (수신기와 동일)
#define SMOKE_SINGLE_MAX_S    15   // 단일 동작 최대 15초
#define SMOKE_STEP_MAX_S      10   // 멀티스텝 개별 스텝 최대 10초
#define MIN_EXECUTION_STEPS   1

#define DEVICE_ID_ADDR        0
#define GROUP_ID_ADDR         1
#define SETTINGS_START_ADDR   32 

// [NEW] 무선 복제 기능 EEPROM 주소 (기존 데이터와 안 겹치게 여유 공간 사용)
#define EEPROM_CLONED_MAC_FLAG 2000
#define EEPROM_CLONED_MAC_ADDR 2001
#define EEPROM_VIB_ENABLED_ADDR 2010  // 진동 ON/OFF 설정
#define EEPROM_LANGUAGE_ADDR     2011  // OLED 표시 언어 (0~6, 미기록 시 영어)

#define MS_PER_SEC            1000UL
#define MS_PER_MIN            (60 * MS_PER_SEC)
#define BUTTON_DEBOUNCE_TIME    50
#define BUTTON_HOLD_TIME       500
#define BUTTON_INITIAL_INTERVAL 300
#define BUTTON_SLOW_INTERVAL    200
#define BUTTON_FAST_INTERVAL    100
#define BLINK_INTERVAL_MS       500
#define DISPLAY_WIDTH         128
#define DISPLAY_HEIGHT        64
#define MAX_CHARS_PER_LINE    21
#define OLED_LINE_HEIGHT      9
#define OLED_MENU_START_Y     16
#define OLED_CURSOR_OFFSET_X  0
constexpr int CURSOR_X = 0;
constexpr int TEXT_X = 10;

#define WIFI_CONNECT_TIMEOUT_MS   15000
#define VOLTAGE_DIVIDER_RATIO 2.0f
#define BATTERY_AVG_SAMPLES   50

#define RGB_LED_PIN           48   // Seeed XIAO ESP32-S3 내장 RGB LED

extern uint8_t WIFI_CHANNEL;
// [FIX] EEPROM_CLONED_MAC_ADDR(2001)+6 = 2001~2006 → 2005가 MAC[4]와 겹쳐 채널 저장 시 MAC 손상.
// 2007로 이동 (MAC 영역 끝 2006 다음, VIB 2010 이전 빈 공간).
#define EEPROM_WIFI_CHANNEL_ADDR 2007
#define RETRY_INTERVAL_MS       120
#define MAX_SEND_ATTEMPTS       5
#define MAX_KNOWN_NETWORKS      5
static const uint8_t broadcastAddress[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

constexpr const char* OTA_VERSION_URL  = "https://mystic-lab.vercel.app/api/firmware/latest?device=nexus_transmitter";

enum class LogLevel { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR };
enum ErrorCode { ERROR_NONE = 0, ERROR_INIT_FAILED, ERROR_INVALID_SETTINGS, ERROR_EXECUTION_FAILED };
enum DeviceType : uint8_t { SINGLE_ACTION = 0, MULTI_ACTION  = 1 };
// [NEW] 실행 스타일: TIMER=설정 타이머대로, HOLD=play 누르는 동안 지속
enum ExecStyle : uint8_t { STYLE_TIMER = 0, STYLE_HOLD = 1 };
enum MachineType : uint8_t { TYPE_ALL = 0, TYPE_POT, TYPE_SMOKE, TYPE_FOUNTAIN, TYPE_REEL, TYPE_MAGNET };

enum Mode {
    MODE_BOOT = 0, MODE_HOME_MENU, MODE_GROUP_EXECUTE_MENU, MODE_SINGLE_EXECUTE_MENU,
    MODE_GROUP_SETTING_OVERVIEW, MODE_GROUP_SETTING_DETAIL, MODE_TIME_SETTING_OVERVIEW,
    MODE_TIME_SETTING_DELAY_EDIT, MODE_TIME_SETTING_PLAY_EDIT, 
    MODE_TIME_SETTING_PWM_EDIT, 
    
    MODE_OTA_WIFI_AP,     
    MODE_OTA_SCANNING,    
    MODE_OTA_CONNECTING,  
    MODE_OTA_CHECKING,    
    MODE_UPDATE_PAGE,     
    MODE_OTA_CONFIRM,     
    MODE_OTA_DOWNLOADING, 
    MODE_OTA_UPDATING,    
    MODE_OTA_SUCCESS,     
    MODE_OTA_ERROR,       
    
    MODE_EXECUTION,
    MODE_TIMER_MONITOR,
    MODE_COMPLETION_MESSAGE,
    MODE_CANCEL_MESSAGE,
    
    // [NEW] 무선 복제 모드
    MODE_CLONE_TX,
    MODE_CLONE_RX,

    // [NEW] RF 자동 환경 분석 스캔 모드
    MODE_RF_SCAN,

    // [NEW] 언어 선택 모드
    MODE_LANGUAGE_SETTING,

    // [NEW] 그룹 멤버 편집 (그룹 실행 화면에서 진입 — 기존 GROUP SET 대체)
    MODE_GROUP_MEMBERS,

    MODE_ERROR
};

enum TimerUnit { UNIT_MINUTES = 0, UNIT_SECONDS, UNIT_PLAY_SECONDS, UNIT_PWM };
enum OledMenuState { FIELD_ID = 0, FIELD_TYPE, FIELD_STEP, FIELD_DELAY, FIELD_PLAY, FIELD_PWM };
enum CommStatus { COMM_IDLE, COMM_PENDING_FIRE, COMM_AWAITING_ACK, COMM_ACK_RECEIVED_SUCCESS, COMM_FAILED_NO_ACK };



struct DeviceSettings {
    uint8_t deviceID;
    DeviceType deviceType;
    MachineType machineType;
    uint8_t groupMembershipBitmask; 
    uint8_t stepCount;
    ExecutionStep steps[MAX_EXECUTION_STEPS];
    bool isValid() const {
        if (stepCount < MIN_EXECUTION_STEPS || stepCount > MAX_EXECUTION_STEPS) return false;
        for (int i = 0; i < stepCount; ++i) {
            if (steps[i].playSeconds < MIN_PLAY_SECONDS || steps[i].playSeconds > MAX_PLAY_SECONDS) return false;
            if (steps[i].pwmValue > 100) return false;
            if (steps[i].delaySeconds > 59) return false;
        }
        return true;
    }
};

struct RunningDevice {
    uint8_t deviceID;
    uint32_t txButtonPressSequenceMicros;
    unsigned long startTimeMs;   // millis() at execution start — used for elapsed time (micros wraps at ~71min)
    CommStatus commStatus;
    uint8_t sendAttempts;
    uint8_t successfulAcks;
    unsigned long lastPacketSendTime;
    uint32_t lastTxTimestamp;
    bool playStarted;
    bool playEnded;
    bool isFinished;
    int8_t currentStepIndex;
};

struct UiState {
    uint8_t menuCursor = 0;
    int scrollOffset = 0;
    bool isAdjustingValue = false;
    TimerUnit adjustingUnit = TimerUnit::UNIT_MINUTES;
    uint8_t selectedStep = 0;
    uint8_t singleExecCursor = 0;
    uint8_t timeSetCursor = 0;
    bool adjustingStepCount = false;
};

struct OtaState {
    bool updateAvailable = false;
    int downloadProgress = 0;
    bool inProgress = false;
    String errorMessage;
    String latestVersion;
    String changeLog;
    String firmwareUrl;
    int scrollOffset = 0;
    bool updateConfirmed = false;
};

extern DeviceSettings deviceSettings[MAX_DEVICES + 1];
extern RunningDevice  runningDevices[MAX_GROUP_DEVICES];
extern uint8_t        groupDeviceCount;
extern portMUX_TYPE   rdMux;
extern volatile Mode currentMode;
extern std::vector<Mode> modeHistory;
extern UiState uiState;
extern OtaState otaState;
extern uint8_t        selectedDevice;
extern uint8_t        selectedGroupNum;
extern uint8_t        previousSelectedDevice;
extern bool           adjustingValue;
extern bool           isProcessing;
extern unsigned long  executionCompleteTime;
extern unsigned long  modeEnteredAt;
extern bool           oledInitialized;
extern bool           espNowInitialized;
extern bool           executionComplete;
extern const String firmwareVersion;
extern const String firmwareNotes;
extern float batteryVoltage;
extern int   batteryPercentage;
extern bool  vibrationEnabled;
extern ExecStyle execStyle; // [NEW] 단일/그룹 실행 공통 스타일(TIMER/HOLD)

extern String wifi_ssid;
extern String wifi_password;
extern String otaErrorMessage;
extern bool otaWorkflowActive;
extern String otaConnectingSsid;

#define WIFI_SSID_ADDR        3000 
#define WIFI_PASS_ADDR        (WIFI_SSID_ADDR + (MAX_KNOWN_NETWORKS * EEPROM_WIFI_SSID_SIZE))
#define EEPROM_WIFI_SSID_SIZE 40
#define EEPROM_WIFI_PASS_SIZE 64  // WPA2 max is 63 chars + null terminator

#endif // CONFIG_T_H