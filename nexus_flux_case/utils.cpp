#include "utils.h"
#include <ArduinoJson.h>
#include <nvs_flash.h>

Log::WsLogSender Log::_wsLogSender = nullptr;
SemaphoreHandle_t Log::_logMutex = NULL;

namespace Utils {
    static const char* PREFS_NAMESPACE = "mystic";
    static const char* KEY_DEVICE_ID = "device_id";
    static const char* KEY_WIFI_CREDS = "wifi_creds";
    static const char* KEY_TEST_DELAY = "test_delay";
    static const char* KEY_TEST_PLAY = "test_play";
    
    static const char* KEY_LAST_SEQ_BLOB = "last_seq_blob"; 
    static const char* KEY_LAST_SEQ_CNT  = "last_seq_cnt";
    
    static const char* KEY_MASTER_MAC = "master_mac"; // [NEW] MAC 키
    static const char* KEY_COMM_CHANNEL = "comm_channel"; // [NEW] RF 채널 키

    static Preferences preferences;

    bool initNvs() {
        if (preferences.begin(PREFS_NAMESPACE, false)) return true;
        // NVS 손상 시 초기화 후 재시도
        nvs_flash_erase();
        nvs_flash_init();
        return preferences.begin(PREFS_NAMESPACE, false);
    }

    String loadWifiCredentials() { return preferences.getString(KEY_WIFI_CREDS, "[]"); }

    String loadWifiPassword(const String& ssid) {
        String currentCreds = loadWifiCredentials();
        JsonDocument doc;
        if (deserializeJson(doc, currentCreds) != DeserializationError::Ok) return "";
        JsonArray array = doc.as<JsonArray>();
        for (JsonObject obj : array) {
            if (obj["ssid"] == ssid) return obj["pass"].as<String>();
        }
        return "";
    }

    void saveWifiCredential(const String& ssid, const String& password) {
        String currentCreds = loadWifiCredentials();
        JsonDocument doc;
        if (deserializeJson(doc, currentCreds) != DeserializationError::Ok || !doc.is<JsonArray>()) {
            doc.to<JsonArray>();
        }
        JsonArray array = doc.as<JsonArray>();

        bool updated = false;
        for (JsonObject obj : array) {
            if (obj["ssid"] == ssid) {
                obj["pass"] = password;
                updated = true;
                break;
            }
        }
        if (!updated) {
            JsonObject newCred = array.add<JsonObject>();
            newCred["ssid"] = ssid;
            newCred["pass"] = password;
        }
        String newCreds;
        serializeJson(doc, newCreds);
        preferences.putString(KEY_WIFI_CREDS, newCreds);
        Log::Info(PSTR("NVS: Saved Wi-Fi credential for %s"), ssid.c_str());
    }

    void removeWifiCredential(const String& ssid) {
        String currentCreds = loadWifiCredentials();
        JsonDocument doc;
        if (deserializeJson(doc, currentCreds) != DeserializationError::Ok || !doc.is<JsonArray>()) {
            doc.to<JsonArray>();
        }
        JsonArray array = doc.as<JsonArray>();
        for (JsonArray::iterator it = array.begin(); it != array.end(); ++it) {
            if ((*it)["ssid"] == ssid) { array.remove(it); break; }
        }
        String newCreds;
        serializeJson(doc, newCreds);
        preferences.putString(KEY_WIFI_CREDS, newCreds);
        Log::Info(PSTR("NVS: Removed Wi-Fi credential for %s"), ssid.c_str());
    }

    void clearAllWifiCredentials() {
        preferences.remove(KEY_WIFI_CREDS);
        Log::Info(PSTR("NVS: Cleared all Wi-Fi credentials"));
    }

    String loadDeviceId() { return preferences.getString(KEY_DEVICE_ID, "1"); }

    void saveDeviceId(const String& id) {
        preferences.putString(KEY_DEVICE_ID, id);
        Log::Info(PSTR("NVS: Saved device ID: %s"), id.c_str());
    }

    int loadTestDelay() { return preferences.getInt(KEY_TEST_DELAY, DEFAULT_TEST_DELAY_MS); }
    void saveTestDelay(int delay) {
        preferences.putInt(KEY_TEST_DELAY, delay);
        Log::Info(PSTR("NVS: Saved test delay: %d ms"), delay);
    }

    int loadTestPlay() { return preferences.getInt(KEY_TEST_PLAY, DEFAULT_TEST_PLAY_MS); }
    void saveTestPlay(int play) {
        preferences.putInt(KEY_TEST_PLAY, play);
        Log::Info(PSTR("NVS: Saved test play duration: %d ms"), play);
    }

    void saveLastSequence(const ExecutionStep* steps, uint8_t count) {
        if (count == 0 || count > MAX_EXECUTION_STEPS) return;
        size_t size = sizeof(ExecutionStep) * count;
        preferences.putBytes(KEY_LAST_SEQ_BLOB, steps, size);
        preferences.putUChar(KEY_LAST_SEQ_CNT, count);
        Log::Info(PSTR("NVS: Saved last sequence. Steps: %d"), count);
    }

    bool loadLastSequence(ExecutionStep* outSteps, uint8_t& outCount) {
        uint8_t count = preferences.getUChar(KEY_LAST_SEQ_CNT, 0);
        if (count == 0 || count > MAX_EXECUTION_STEPS) return false;
        size_t size = sizeof(ExecutionStep) * count;
        size_t readLen = preferences.getBytes(KEY_LAST_SEQ_BLOB, outSteps, size);
        if (readLen != size) {
            Log::Warn(PSTR("NVS: Failed to load sequence blob completely."));
            return false;
        }
        outCount = count;
        Log::Info(PSTR("NVS: Loaded last sequence. Steps: %d"), count);
        return true;
    }

    uint32_t loadTimedRunMs() {
        return preferences.getUInt("timed_run_ms", DEFAULT_TIMED_RUN_MS);
    }

    void saveTimedRunMs(uint32_t ms) {
        preferences.putUInt("timed_run_ms", ms);
        Log::Info(PSTR("NVS: Saved timed run duration: %lu ms"), ms);
    }

    // [NEW] 송신기 MAC 저장 함수
    void saveMasterMac(const uint8_t* mac) {
        preferences.putBytes(KEY_MASTER_MAC, mac, 6);
        Log::Info(PSTR("NVS: Saved Master Transmitter MAC Address."));
    }

    bool loadMasterMac(uint8_t* mac) {
        if (preferences.getBytesLength(KEY_MASTER_MAC) == 6) {
            preferences.getBytes(KEY_MASTER_MAC, mac, 6);
            return true;
        }
        return false;
    }

    void saveCommChannel(uint8_t channel) {
        preferences.putUChar(KEY_COMM_CHANNEL, channel);
        Log::Info(PSTR("NVS: Saved Comm Channel: %d"), channel);
    }

    uint8_t loadCommChannel() {
        return preferences.getUChar(KEY_COMM_CHANNEL, 1); // 기본값 1
    }
}

void Log::begin() { _logMutex = xSemaphoreCreateMutex(); }
void Log::setWebSocketLogSender(const WsLogSender& sender) { _wsLogSender = sender; }
void Log::printLog(const char* level, const char* format, va_list args) {
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        if (Serial) Serial.printf("[%lu ms][%s] %s\n", millis(), level, buffer);
        if (_wsLogSender) _wsLogSender(level, String(buffer));
        xSemaphoreGive(_logMutex);
    }
}
void Log::Info(const char* format, ...) { va_list args; va_start(args, format); printLog("INFO", format, args); va_end(args); }
void Log::Warn(const char* format, ...) { va_list args; va_start(args, format); printLog("WARN", format, args); va_end(args); }
void Log::Error(const char* format, ...) { va_list args; va_start(args, format); printLog("ERROR", format, args); va_end(args); }
void Log::Debug(const char* format, ...) {
#if DEBUG_MODE
    va_list args; va_start(args, format); printLog("DEBUG", format, args); va_end(args);
#endif
}
void Log::TestLog(const char* format, ...) {
    va_list args; va_start(args, format); printLog("TEST", format, args); va_end(args);
}