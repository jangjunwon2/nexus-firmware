// transmitter/utils_t.cpp
#include "utils_t.h"
#include "config_t.h" 
#include <EEPROM.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

std::vector<NetworkCred> knownNetworks;

//────────────────────────────────────────────────────────────────────────────
// 1) Logging Functions
//────────────────────────────────────────────────────────────────────────────
void initLog() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\nLogging initialized");
}

#if DEBUG_MODE
void logPrintf(LogLevel level, const char* format, ...) {
    if (!Serial) return;
    
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    const char* levelStr;
    switch (level) {
        case LogLevel::LOG_DEBUG: levelStr = "DEBUG"; break;
        case LogLevel::LOG_INFO:  levelStr = "INFO";  break;
        case LogLevel::LOG_WARN:  levelStr = "WARN";  break;
        case LogLevel::LOG_ERROR: levelStr = "ERROR"; break;
        default: levelStr = "UNKNOWN";
    }
    
    Serial.printf("[%s] %s\n", levelStr, buffer);
}
#else
// DEBUG_MODE가 0일 때, logPrintf를 아무것도 하지 않는 빈 함수로 만듭니다.
#endif

//────────────────────────────────────────────────────────────────────────────
// 2) Timer Calculation Functions
//────────────────────────────────────────────────────────────────────────────
uint32_t getTimerMs(uint8_t deviceID, bool isDelay, uint8_t stepIndex) {
    if (deviceID > MAX_DEVICES || stepIndex >= MAX_EXECUTION_STEPS) {
        return 0;
    }
    
    const DeviceSettings& settings = deviceSettings[deviceID];
    const ExecutionStep& step = settings.steps[stepIndex];
    
    if (isDelay) {
        return (uint32_t)(step.delayMinutes) * MS_PER_MIN + (uint32_t)(step.delaySeconds) * MS_PER_SEC;
    } else {
        return (uint32_t)step.playSeconds * MS_PER_SEC;
    }
}

//────────────────────────────────────────────────────────────────────────────
// 3) EEPROM Initialization and ID/Group ID Management
//────────────────────────────────────────────────────────────────────────────
void initEEPROM() {
    EEPROM.begin(EEPROM_SIZE);
    delay(100);
    logPrintf(LogLevel::LOG_INFO, "EEPROM initialized");
}

uint8_t loadID() {
    return EEPROM.read(DEVICE_ID_ADDR);
}

void saveID(uint8_t id) {
    EEPROM.write(DEVICE_ID_ADDR, id);
    EEPROM.commit();
}

uint8_t loadGroupID() {
    return EEPROM.read(GROUP_ID_ADDR);
}

void saveGroupID(uint8_t groupId) {
    EEPROM.write(GROUP_ID_ADDR, groupId);
    EEPROM.commit();
}

//────────────────────────────────────────────────────────────────────────────
// 4) Settings (Load/Save) Functions
//────────────────────────────────────────────────────────────────────────────
void loadSettings() {
    for (uint8_t i = 1; i <= MAX_DEVICES; i++) {
        uint16_t baseAddr = SETTINGS_START_ADDR + ((i-1) * sizeof(DeviceSettings));
        EEPROM.get(baseAddr, deviceSettings[i]);

        // 읽어온 데이터가 유효하지 않으면 기본값으로 설정하고 저장
        if (!deviceSettings[i].isValid()) {
            logPrintf(LogLevel::LOG_WARN, "Device %d: Invalid settings in EEPROM, applying defaults.", i);
            deviceSettings[i].deviceID = i;
            deviceSettings[i].deviceType = SINGLE_ACTION;
            deviceSettings[i].machineType = TYPE_ALL; // [NEW] 기본값 ALL (무조건 동작)
            deviceSettings[i].groupMembershipBitmask = 0;
            deviceSettings[i].stepCount = 1;
            for(int j=0; j < MAX_EXECUTION_STEPS; ++j) {
                deviceSettings[i].steps[j] = {0, 0, MIN_PLAY_SECONDS, 100};
            }
            EEPROM.put(baseAddr, deviceSettings[i]);
        }
    }
    EEPROM.commit();
    logPrintf(LogLevel::LOG_INFO, "Settings loaded from EEPROM");
}

void saveSettings(bool saveGroupOnly) {
    uint16_t baseAddr = SETTINGS_START_ADDR + ((selectedDevice-1) * sizeof(DeviceSettings));
    if (saveGroupOnly) {
        DeviceSettings tempSettings;
        EEPROM.get(baseAddr, tempSettings);
        tempSettings.groupMembershipBitmask = deviceSettings[selectedDevice].groupMembershipBitmask;
        EEPROM.put(baseAddr, tempSettings);
        logPrintf(LogLevel::LOG_INFO, "Group setting for ID %d saved. Bitmask: 0x%02X", selectedDevice, tempSettings.groupMembershipBitmask);
    } else {
        EEPROM.put(baseAddr, deviceSettings[selectedDevice]);
        logPrintf(LogLevel::LOG_INFO, "Timer settings for ID %d saved.", selectedDevice);
    }
    EEPROM.commit();
}

//----------------------------------------------------------------------------
// [NEW] Known Wi-Fi Network Management
//----------------------------------------------------------------------------
void loadKnownNetworks() {
    // EEPROM은 이미 initEEPROM()에서 begin됨
    knownNetworks.clear();
    for(int i = 0; i < MAX_KNOWN_NETWORKS; i++) {
        int base = WIFI_SSID_ADDR + i * (EEPROM_WIFI_SSID_SIZE + EEPROM_WIFI_PASS_SIZE);
        char ssid_buf[EEPROM_WIFI_SSID_SIZE + 1] = {0};
        char pass_buf[EEPROM_WIFI_PASS_SIZE + 1] = {0};
        
        EEPROM.readBytes(base, ssid_buf, EEPROM_WIFI_SSID_SIZE);
        EEPROM.readBytes(base + EEPROM_WIFI_SSID_SIZE, pass_buf, EEPROM_WIFI_PASS_SIZE);

        String ssid(ssid_buf);
        if(ssid.length() > 0 && isprint(ssid.charAt(0))) {
             knownNetworks.push_back({ssid, String(pass_buf)});
        }
    }
    logPrintf(LogLevel::LOG_INFO, "OTA: Loaded %d known networks from EEPROM.", knownNetworks.size());
}

void saveKnownNetwork(const String& ssid, const String& pass) {
    for (auto &net : knownNetworks) {
        if (net.ssid == ssid) {
            net.pass = pass;
            commitKnownNetworks();
            logPrintf(LogLevel::LOG_INFO, "OTA: Updated password for known network: %s", ssid.c_str());
            return;
        }
    }

    if (knownNetworks.size() >= MAX_KNOWN_NETWORKS) {
        logPrintf(LogLevel::LOG_INFO, "OTA: Network storage full. Replacing oldest network '%s' with '%s'.", knownNetworks.front().ssid.c_str(), ssid.c_str());
        knownNetworks.erase(knownNetworks.begin());
    }
    knownNetworks.push_back({ssid, pass});
    logPrintf(LogLevel::LOG_INFO, "OTA: Added new network: '%s'. Total: %d/%d", ssid.c_str(), knownNetworks.size(), MAX_KNOWN_NETWORKS);

    commitKnownNetworks();
}

void commitKnownNetworks() {
    uint8_t clear_buf[EEPROM_WIFI_SSID_SIZE + EEPROM_WIFI_PASS_SIZE] = {0};
    for(int i = 0; i < MAX_KNOWN_NETWORKS; ++i) {
        int base = WIFI_SSID_ADDR + i * (EEPROM_WIFI_SSID_SIZE + EEPROM_WIFI_PASS_SIZE);
        EEPROM.writeBytes(base, clear_buf, sizeof(clear_buf));
    }
    
    for(int i = 0; i < knownNetworks.size(); i++) {
        int base = WIFI_SSID_ADDR + i * (EEPROM_WIFI_SSID_SIZE + EEPROM_WIFI_PASS_SIZE);
        EEPROM.writeString(base, knownNetworks[i].ssid);
        EEPROM.writeString(base + EEPROM_WIFI_SSID_SIZE, knownNetworks[i].pass);
    }
    EEPROM.commit();
    logPrintf(LogLevel::LOG_INFO, "OTA: Committed %d known networks to EEPROM.", knownNetworks.size());
}

void clearKnownNetworks() {
    uint8_t clear_buf[MAX_KNOWN_NETWORKS * (EEPROM_WIFI_SSID_SIZE + EEPROM_WIFI_PASS_SIZE)] = {0};
    EEPROM.writeBytes(WIFI_SSID_ADDR, clear_buf, sizeof(clear_buf));
    EEPROM.commit();
    knownNetworks.clear();
    logPrintf(LogLevel::LOG_INFO, "OTA: Cleared all known networks from EEPROM and memory.");
}

// [NEW] 디버깅을 위한 모드 이름 변환 함수
const char* getModeString(int mode) {
    switch(mode) {
        case MODE_BOOT: return "BOOT";
        case MODE_HOME_MENU: return "HOME_MENU";
        case MODE_GROUP_EXECUTE_MENU: return "GROUP_EXEC_MENU";
        case MODE_SINGLE_EXECUTE_MENU: return "SINGLE_EXEC_MENU";
        case MODE_GROUP_SETTING_OVERVIEW: return "GROUP_SET_OVERVIEW";
        case MODE_GROUP_SETTING_DETAIL: return "GROUP_SET_DETAIL";
        case MODE_TIME_SETTING_OVERVIEW: return "TIME_SET_OVERVIEW";
        case MODE_TIME_SETTING_DELAY_EDIT: return "TIME_SET_DELAY_EDIT";
        case MODE_TIME_SETTING_PLAY_EDIT: return "TIME_SET_PLAY_EDIT";
        case MODE_TIME_SETTING_PWM_EDIT: return "TIME_SET_PWM_EDIT";
        case MODE_EXECUTION: return "EXECUTION";
        case MODE_TIMER_MONITOR: return "TIMER_MONITOR";
        case MODE_COMPLETION_MESSAGE: return "COMPLETION_MSG";
        case MODE_ERROR: return "ERROR";
        // OTA 관련
        case MODE_OTA_WIFI_AP: return "OTA_WIFI_AP";
        case MODE_OTA_SCANNING: return "OTA_SCANNING";
        case MODE_OTA_CONNECTING: return "OTA_CONNECTING";
        case MODE_UPDATE_PAGE: return "OTA_UPDATE_PAGE";
        case MODE_OTA_CONFIRM: return "OTA_CONFIRM";
        case MODE_OTA_DOWNLOADING: return "OTA_DOWNLOADING";
        case MODE_OTA_ERROR: return "OTA_ERROR";
        case MODE_CLONE_TX: return "SYNC (MAIN)"; // 추가
        case MODE_CLONE_RX: return "SYNC (SPARE)"; // 추가
        default: return "UNKNOWN_MODE";
    }
}