#include "config.h"
#include "utils.h"
#include "hardware.h"
#include "comm.h"
#include "mode.h"
#include "web.h"

// 전역 인스턴스
HardwareManager hwManager;
CommManager commManager;
WebManager webManager;
ModeManager modeManager(&hwManager, &commManager, &webManager);

void setup() {
    Serial.begin(115200);
    delay(100);

    Log::begin();
    Log::Info(PSTR("=========================================="));
    Log::Info(PSTR("     Nexus Pot v%s"), FIRMWARE_VERSION);
    Log::Info(PSTR("=========================================="));

    // NVS 초기화
    if (!Utils::initNvs()) {
        Log::Error(PSTR("SETUP: Failed to initialize NVS."));
        // 무한 루프 또는 재시작
        while(true);
    }

    // [FIXED] NVS에서 Device ID를 먼저 로드합니다.
    uint8_t deviceId = Utils::loadDeviceId().toInt();

    // 관리자 클래스 초기화
    hwManager.begin();

    // [FIXED] begin 함수에 로드한 deviceId를 전달합니다.
    if (!commManager.begin(deviceId, &modeManager)) {
        Log::Error(PSTR("SETUP: Failed to initialize CommManager."));
        modeManager.switchToMode(DeviceMode::MODE_ERROR);
    }
    
    webManager.begin(&modeManager, &commManager);
    modeManager.begin();

    // 초기 모드를 NORMAL으로 설정
    modeManager.switchToMode(DeviceMode::MODE_NORMAL);
    
    Log::Info(PSTR("SETUP: System initialized. Welcome!"));
    enableWatchdog(WDT_TIMEOUT_S);
}

void loop() {
    // [FIXED] ModeManager의 loop()는 update()로 변경되어야 합니다.
    modeManager.update();

    // 워치독 타이머 리셋
    esp_task_wdt_reset(); 
    
    // 다른 태스크가 실행될 시간을 확보합니다.
    vTaskDelay(pdMS_TO_TICKS(10)); 
}