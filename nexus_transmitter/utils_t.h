// transmitter/utils_t.h
#pragma once
#ifndef UTILS_T_H
#define UTILS_T_H

#include <Arduino.h>
#include "config_t.h" // LogLevel 열거형을 가져오기 위해 포함
#include <EEPROM.h>
#include <vector>

// [FIXED] LogLevel의 중복 정의를 막기 위해 이 파일에서는 정의를 제거합니다.
// 이제 config_t.h에만 정의되어 있습니다.

//────────────────────────────────────────────────────────────────────────────
// 1) Logging Functions
//────────────────────────────────────────────────────────────────────────────
void initLog();

// ▼▼▼ 수정된 부분 ▼▼▼
#if DEBUG_MODE
void logPrintf(LogLevel level, const char* format, ...);
#else
// DEBUG_MODE가 0일 때, logPrintf를 아무것도 하지 않는 빈 함수로 만듭니다.
#define logPrintf(...) do {} while(0)
#endif
// ▲▲▲ 수정된 부분 ▲▲▲

//────────────────────────────────────────────────────────────────────────────
// 2) Timer Calculation Functions
//────────────────────────────────────────────────────────────────────────────
// [MODIFIED] stepIndex 파라미터 추가
uint32_t getTimerMs(uint8_t deviceID, bool isDelay, uint8_t stepIndex);

//────────────────────────────────────────────────────────────────────────────
// 3) EEPROM Initialization and ID/Group ID Management
//────────────────────────────────────────────────────────────────────────────
void initEEPROM();
uint8_t loadID();
void saveID(uint8_t id);
uint8_t loadGroupID();
void saveGroupID(uint8_t groupId);

//────────────────────────────────────────────────────────────────────────────
// 4) Settings (Load/Save) Functions
//────────────────────────────────────────────────────────────────────────────
void loadSettings();
void saveSettings(bool saveGroupOnly);

struct NetworkCred {
    String ssid;
    String pass;
};

extern std::vector<NetworkCred> knownNetworks;

void loadKnownNetworks();
void saveKnownNetwork(const String& ssid, const String& pass);
void commitKnownNetworks();
void clearKnownNetworks();

// [NEW] 디버깅을 위한 모드 문자열 변환 함수 선언
const char* getModeString(int mode);

inline bool isVersionNewer(const String& latest, const String& current) {
    if (latest.isEmpty() || current.isEmpty() || latest == "N/A" || latest == current) return false;
    int current_idx = 0, latest_idx = 0;
    while (current_idx < (int)current.length() || latest_idx < (int)latest.length()) {
        long current_val = 0;
        int current_part_end = current.indexOf('.', current_idx);
        if (current_part_end == -1) current_part_end = current.length();
        if (current_idx < (int)current.length()) current_val = current.substring(current_idx, current_part_end).toInt();

        long latest_val = 0;
        int latest_part_end = latest.indexOf('.', latest_idx);
        if (latest_part_end == -1) latest_part_end = latest.length();
        if (latest_idx < (int)latest.length()) latest_val = latest.substring(latest_idx, latest_part_end).toInt();

        if (latest_val > current_val) return true;
        if (latest_val < current_val) return false;
        current_idx = current_part_end + 1;
        latest_idx = latest_part_end + 1;
    }
    return false;
}

#endif // UTILS_T_H