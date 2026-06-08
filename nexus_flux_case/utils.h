#pragma once
#ifndef UTILS_H
#define UTILS_H

#include <Arduino.h>
#include <Preferences.h>
#include <functional>
#include <esp_task_wdt.h>
#include "config.h"
#include <ArduinoJson.h>

class WebManager;

class Log {
public:
    using WsLogSender = std::function<void(const char* level, const String& msg)>;
    static void begin();
    static void Info(const char* format, ...);
    static void Warn(const char* format, ...);
    static void Error(const char* format, ...);
    static void Debug(const char* format, ...);
    static void TestLog(const char* format, ...);

private:
    friend class WebManager;
    static void setWebSocketLogSender(const WsLogSender& sender);
    static void printLog(const char* level, const char* format, va_list args);
    static WsLogSender _wsLogSender;
    static SemaphoreHandle_t _logMutex;
};

inline void enableWatchdog(uint32_t timeoutS = WDT_TIMEOUT_S) {
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = timeoutS * 1000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };
    
    esp_err_t init_result = esp_task_wdt_init(&wdt_config);
    if (init_result == ESP_ERR_INVALID_STATE) {
        if (esp_task_wdt_reconfigure(&wdt_config) != ESP_OK) {
             ets_printf("ERROR: Failed to reconfigure WDT\n");
        }
    } else if (init_result != ESP_OK) {
        ets_printf("ERROR: Failed to init WDT: %s\n", esp_err_to_name(init_result));
    }

    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ets_printf("ERROR: Failed to add loop task to WDT\n");
    }
}

namespace Utils {
    bool initNvs();
    void saveWifiCredential(const String& ssid, const String& password);
    void removeWifiCredential(const String& ssid);
    String loadWifiCredentials();
    String loadWifiPassword(const String& ssid);
    void clearAllWifiCredentials();
    String loadDeviceId();
    void saveDeviceId(const String& deviceId);
    int loadTestDelay();
    void saveTestDelay(int delayMs);
    int loadTestPlay();
    void saveTestPlay(int playMs);

    void saveLastSequence(const ExecutionStep* steps, uint8_t count);
    bool loadLastSequence(ExecutionStep* outSteps, uint8_t& outCount);

    // [NEW] 송신기 MAC 주소 저장/불러오기
    void saveMasterMac(const uint8_t* mac);
    bool loadMasterMac(uint8_t* mac);

    uint32_t loadTimedRunMs();
    void saveTimedRunMs(uint32_t ms);
}

constexpr size_t JSON_DOC_SIZE_WS_LOG = 384;
constexpr size_t JSON_DOC_SIZE_STATUS = 768;
constexpr size_t JSON_DOC_SIZE_API_RESP = 256;
constexpr size_t JSON_DOC_SIZE_WIFI_SCAN = 2048;

inline bool isVersionNewer(const String& latest, const String& current) {
    if (latest.isEmpty() || current.isEmpty() || latest == "N/A" || latest == current) return false;
    int current_idx = 0, latest_idx = 0;
    while (current_idx < current.length() || latest_idx < latest.length()) {
        long current_val = 0;
        int current_part_end = current.indexOf('.', current_idx);
        if (current_part_end == -1) current_part_end = current.length();
        if (current_idx < current.length()) current_val = current.substring(current_idx, current_part_end).toInt();
        
        long latest_val = 0;
        int latest_part_end = latest.indexOf('.', latest_idx);
        if (latest_part_end == -1) latest_part_end = latest.length();
        if (latest_idx < latest.length()) latest_val = latest.substring(latest_idx, latest_part_end).toInt();
        
        if (latest_val > current_val) return true;
        if (latest_val < current_val) return false;
        current_idx = current_part_end + 1;
        latest_idx = latest_part_end + 1;
    }
    return false;
}

#endif // UTILS_H