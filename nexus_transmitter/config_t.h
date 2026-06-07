// transmitter_s3/config_t.h

#ifndef CONFIG_T_H
#define CONFIG_T_H

#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <vector>
#include "espnow_comm_shared.h"

#define DEBUG_MODE 1 

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
#define HOME_MENU_ITEM_COUNT  7 // [MODIFIED] CLONE TX, CLONE RX 메뉴 추가로 7개
#define EEPROM_SIZE           4096 
#define MAX_DELAY_MINUTES     59
#define MAX_DELAY_SECONDS     59
#define MIN_PLAY_SECONDS      1
#define MAX_PLAY_SECONDS      60
#define MIN_EXECUTION_STEPS   1

#define DEVICE_ID_ADDR        0
#define GROUP_ID_ADDR         1
#define SETTINGS_START_ADDR   32 

// [NEW] 무선 복제 기능 EEPROM 주소 (기존 데이터와 안 겹치게 여유 공간 사용)
#define EEPROM_CLONED_MAC_FLAG 2000  
#define EEPROM_CLONED_MAC_ADDR 2001  

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
const int CURSOR_X = 0;
const int TEXT_X = 10;

#define WIFI_CONNECT_TIMEOUT_MS   5000
#define ADC_MAX_READING       4095
#define ADC_REF_VOLTAGE       3.3  
#define VOLTAGE_DIVIDER_RATIO 2.0f 
#define BATTERY_MAX_VOLTAGE   4.15f 
#define BATTERY_MIN_VOLTAGE   3.20f
#define BATTERY_AVG_SAMPLES   50

#define WIFI_CHANNEL            1
#define ACK_TIMEOUT_MS          200
#define RETRY_INTERVAL_MS       300
#define MAX_SEND_ATTEMPTS       9
#define MAX_KNOWN_NETWORKS      5
static const uint8_t broadcastAddress[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define OTA_VERSION_URL "https://mystic-lab.vercel.app/api/admin/firmware/version?device=nexus_transmitter"

enum class LogLevel { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR };
enum ErrorCode { ERROR_NONE = 0, ERROR_INIT_FAILED, ERROR_INVALID_SETTINGS, ERROR_EXECUTION_FAILED };
enum DeviceType : uint8_t { SINGLE_ACTION = 0, MULTI_ACTION  = 1 };
enum MachineType : uint8_t { TYPE_ALL = 0, TYPE_POT, TYPE_SMOKE, TYPE_FOUNTAIN, TYPE_REEL, TYPE_MAGNET };

enum Mode {
    MODE_BOOT = 0, MODE_HOME_MENU, MODE_GROUP_EXECUTE_MENU, MODE_SINGLE_EXECUTE_MENU,
    MODE_GROUP_SETTING_OVERVIEW, MODE_GROUP_SETTING_DETAIL, MODE_TIME_SETTING_OVERVIEW,
    MODE_TIME_SETTING_DEVICE_TYPE, MODE_MULTI_ACTION_OVERVIEW, MODE_MULTI_ACTION_DETAIL,
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
    
    // [NEW] 무선 복제 모드
    MODE_CLONE_TX,  
    MODE_CLONE_RX,  
    
    MODE_ERROR
};

enum TimerUnit { UNIT_MINUTES = 0, UNIT_SECONDS, UNIT_PLAY_SECONDS, UNIT_PWM };
enum OledMenuState { FIELD_ID = 0, FIELD_TYPE, FIELD_STEP, FIELD_DELAY, FIELD_PLAY, FIELD_PWM };
enum CommStatus { COMM_IDLE, COMM_PENDING_RTT_REQUEST, COMM_AWAITING_RTT_ACK, COMM_PENDING_FINAL_COMMAND, COMM_AWAITING_FINAL_ACK, COMM_ACK_RECEIVED_SUCCESS, COMM_FAILED_NO_ACK };



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
        }
        return true;
    }
};

struct RunningDevice {
    uint8_t deviceID;
    uint32_t txButtonPressSequenceMicros;
    CommStatus commStatus;
    uint8_t sendAttempts;
    uint8_t successfulAcks;
    unsigned long lastPacketSendTime;
    unsigned long ackTimeoutDeadline;
    uint32_t lastTxTimestamp;
    uint32_t currentSequenceRttUs;
    uint32_t currentSequenceRxProcessingTimeUs;
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
extern Mode currentMode;
extern std::vector<Mode> modeHistory;
extern UiState uiState;
extern OtaState otaState;
extern uint8_t        selectedDevice;
extern uint8_t        selectedGroupNum;
extern uint8_t        previousSelectedDevice;
extern bool           adjustingValue;
extern bool           isProcessing;
extern unsigned long  executionCompleteTime;
extern bool           oledInitialized;
extern bool           espNowInitialized;
extern bool           executionComplete;
extern const String firmwareVersion;
extern float batteryVoltage;
extern int   batteryPercentage;

extern String wifi_ssid;
extern String wifi_password;
extern String otaErrorMessage;
extern bool otaWorkflowActive;
extern String otaConnectingSsid;

#define WIFI_SSID_ADDR        3000 
#define WIFI_PASS_ADDR        (WIFI_SSID_ADDR + (MAX_KNOWN_NETWORKS * EEPROM_WIFI_SSID_SIZE))
#define EEPROM_WIFI_SSID_SIZE 40
#define EEPROM_WIFI_PASS_SIZE 40

#endif // CONFIG_T_H