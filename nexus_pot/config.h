/**
 * @file config.h
 * @brief 모든 프로젝트 상수 및 설정을 위한 중앙 설정 파일입니다.
 */
#pragma once
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <IPAddress.h>
#include "espnow_comm_shared.h"

// --- 펌웨어 버전 ---
constexpr const char* FIRMWARE_VERSION = "1.1";

// --- 디버깅 ---
constexpr bool DEBUG_MODE = true;

// --- 워치독 타이머 ---
constexpr uint32_t WDT_TIMEOUT_S = 30;

// --- GPIO 핀 정의 ---
constexpr uint8_t ID_BUTTON_PIN = 5;
constexpr uint8_t EXEC_BUTTON_PIN = 4;
constexpr uint8_t MOSFET_PIN_1 = 7;
constexpr uint8_t MOSFET_PIN_2 = 12;
constexpr uint8_t LED_PIN = 48;

// --- 버튼 타이밍 및 설정 ---
constexpr unsigned long DEBOUNCE_DELAY_MS = 50;
constexpr unsigned long SHORT_PRESS_THRESHOLD_MS = 2000;
constexpr unsigned long LONG_PRESS_THRESHOLD_MS = 2000;
constexpr unsigned long ID_SET_TIMEOUT_MS = 5000;

// --- LED 타이밍 및 패턴 ---
constexpr unsigned long LED_ID_BLINK_INTERVAL_MS = 200;
constexpr unsigned long LED_ID_SET_ENTER_ON_MS = 1000;
constexpr unsigned long LED_ID_SET_INCREMENT_BLINK_MS = 100;
constexpr unsigned long LED_ID_SET_CONFIRM_ON_MS = 1000;
constexpr unsigned long LED_WIFI_MODE_BLINK_INTERVAL_MS = 500;
constexpr uint8_t LED_WIFI_MODE_BLINK_COUNT = 3;
constexpr unsigned long LED_BOOT_SUCCESS_ON_MS = 1000;

// --- 장치 ID 및 실행 설정 ---
constexpr uint8_t DEFAULT_DEVICE_ID = 1;
constexpr uint8_t MIN_DEVICE_ID = 1;
constexpr uint8_t MAX_DEVICE_ID = 20; 

// --- ESP-NOW 설정 ---
constexpr uint8_t ESP_NOW_CHANNEL = 1;
static const uint8_t BROADCAST_ADDRESS[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// --- WI-FI 및 WEB UI 설정 ---
constexpr const char* AP_SSID = "Nexus_Pot";
constexpr const char* AP_PASSWORD = "";
#define AP_IP                   IPAddress(192, 168, 4, 1) 
constexpr unsigned long WIFI_MODE_AUTO_EXIT_MS = (5 * 60 * 1000);
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000; 

// --- OTA 업데이트 URL ---
constexpr const char* OTA_VERSION_URL = "https://mystic-lab.vercel.app/api/firmware/latest?device=nexus_pot";
constexpr unsigned long OTA_HTTP_TIMEOUT_MS = 10000;

// --- 테스트 모드 설정 ---
constexpr uint32_t DEFAULT_TEST_DELAY_MS = 0;
constexpr uint32_t DEFAULT_TEST_PLAY_MS = 1000;

// ====================================================================================
//                            전역 열거형 및 구조체
// ====================================================================================

enum MachineType : uint8_t {
    TYPE_ALL = 0,
    TYPE_POT,
    TYPE_SMOKE,
    TYPE_FOUNTAIN,
    TYPE_REEL,
    TYPE_MAGNET
};

// [SETTING] 이 펌웨어가 올라가는 장치의 타입 설정
constexpr MachineType MY_MACHINE_TYPE = TYPE_POT;



enum class DeviceMode {
    MODE_BOOT,
    MODE_NORMAL,
    MODE_ID_BLINK,
    MODE_ID_SET,
    MODE_WIFI,
    MODE_TEST,
    MODE_EXIT_WIFI,
    MODE_PAIRING, // [NEW] 무선 페어링 모드
    MODE_ERROR
};

enum class ButtonEventType {
    NO_EVENT,
    ID_BUTTON_SHORT_PRESS,
    ID_BUTTON_LONG_PRESS_END,
    EXEC_BUTTON_PRESS,
    EXEC_BUTTON_RELEASE,
    BOTH_BUTTONS_LONG_PRESS
};

enum class LedPatternType {
    LED_OFF,
    LED_ON,
    LED_BOOT_SUCCESS,
    LED_ID_DISPLAY,
    LED_ID_SET_ENTER,
    LED_ID_SET_INCREMENT,
    LED_ID_SET_CONFIRM,
    LED_WIFI_MODE_TOGGLE,
    LED_PAIRING, // [NEW] 무선 페어링 대기 패턴
    LED_ERROR
};

#endif // CONFIG_H