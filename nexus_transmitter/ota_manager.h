// transmitter/ota_manager.h

#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "config_t.h"

#define OTA_AP_PREFIX "Nexus Transmitter"
#define OTA_AP_CHANNEL 1
#define OTA_AP_MAX_CONN 2
#define OTA_AP_IP IPAddress(192, 168, 4, 1)
#define OTA_AP_SUBNET IPAddress(255, 255, 255, 0)

// --- 함수 선언부 ---
void initOtaWorkflow();    // OTA 모드 진입 (AP 시작)
void handleOtaLoop();      // 메인 루프에서 처리할 OTA 로직
void shutdownWifi();       // Wi-Fi 종료
void setupWebServer();     // 웹 서버 설정 (업로드 핸들러 포함)

// [NEW] OTA 모드 확인 함수
inline bool isOtaMode(Mode mode) {
    return (mode == MODE_OTA_WIFI_AP || mode == MODE_OTA_UPDATING || mode == MODE_OTA_SUCCESS || mode == MODE_OTA_ERROR);
}

#endif // OTA_MANAGER_H