// transmitter/config_t.cpp

#include "config_t.h"

//────────────────────────────────────────────────────────────────────────────
// Global Variable Definitions (전역 변수 정의)
//────────────────────────────────────────────────────────────────────────────

// 장치 설정 및 상태
DeviceSettings deviceSettings[MAX_DEVICES + 1];
RunningDevice  runningDevices[MAX_GROUP_DEVICES];
uint8_t        groupDeviceCount = 0;
portMUX_TYPE   rdMux = portMUX_INITIALIZER_UNLOCKED; // runningDevices commStatus 크로스코어 spinlock

// 모드 및 UI 상태
volatile Mode currentMode = MODE_BOOT;
std::vector<Mode> modeHistory;
UiState uiState; // menuState, menuCursor 등이 이 구조체 안에 포함됨
OtaState otaState; // 모든 OTA 관련 상태 변수들이 이 구조체 안에 포함됨

// UI 선택 관련 변수
uint8_t        selectedDevice = 1;
uint8_t        selectedGroupNum = 1;
uint8_t        previousSelectedDevice = 0;

// 값 편집 관련 변수
bool           adjustingValue = false;

// 실행 로직 관련 변수
bool           isProcessing = false;
unsigned long  executionCompleteTime = 0;
unsigned long  modeEnteredAt = 0;
bool           executionComplete = false;

// 하드웨어 및 통신 초기화 상태
bool           oledInitialized = false;
bool           espNowInitialized = false;

// 펌웨어 및 Wi-Fi 정보
const String firmwareVersion = FIRMWARE_VERSION;
const String firmwareNotes   = FIRMWARE_NOTES;
String wifi_ssid = "";
String wifi_password = "";
String otaErrorMessage = "";
bool otaWorkflowActive = false;
String otaConnectingSsid = "";

// 배터리 상태
float batteryVoltage = 0.0;
int   batteryPercentage = 0;

// 진동 모터 설정
bool  vibrationEnabled = true;

// [NEW] 실행 스타일 (TIMER 기본). 단일/그룹 실행 화면에서 토글.
ExecStyle execStyle = STYLE_TIMER;

// [NEW] ESP-NOW 전송 시간 기록 (배터리 측정 타이밍 조정용)
unsigned long lastEspNowTxTime = 0;