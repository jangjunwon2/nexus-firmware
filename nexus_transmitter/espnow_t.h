#ifndef ESPNOW_T_H
#define ESPNOW_T_H

#include "config_t.h"
#include "espnow_comm_shared.h"
#include <esp_now.h>
#include <WiFi.h>

extern bool executionComplete; 
extern unsigned long lastEspNowTxTime; 

bool initEspNow();
void reinitEspNow();
void espNowSendCb(const esp_now_send_info_t *info, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len);
bool manageCommunication();
void sendFireBurst(RunningDevice& device); // [NEW] FIRE 4회 버스트 송출 (초기/재시도 공용)
void sendHoldCommand(uint8_t targetId, uint8_t machineType, uint8_t pwm, uint8_t active); // [NEW] 홀드 킵얼라이브 1회 송출
void sendCancelCommand(uint8_t targetId);  // [NEW] 실행 취소 명령 3회 버스트 송출
void deinitEspNow();

// [NEW] 무선 복제 송신 함수 선언
void sendCloneAnnounce();

// [NEW] RF 자동 환경 분석 관련 함수 및 전역 변수 선언
void startRfScanSequence();
void stopRfScanSequence();

enum RfScanStatus : uint8_t {
    RF_SCAN_IDLE = 0,
    RF_SCAN_INIT,
    RF_SCAN_SYNCING,
    RF_SCAN_AWAITING,
    RF_SCAN_SUCCESS,
    RF_SCAN_TIMEOUT
};

extern volatile bool rfScanComplete;
extern volatile bool rfScanRunning;
extern volatile uint8_t rfScanBestChannel;
extern volatile uint8_t rfScanChannelRates[3];
extern volatile RfScanStatus rfScanStatus;

// Wi-Fi 콜백 밖에서 안전하게 EEPROM/재부팅 처리하기 위한 플래그
extern volatile bool cloneReceivedFlag;
extern volatile uint8_t clonedMacBuffer[6];

// [NEW] SPARE COPY 수신 진행 상태 (CLONE_RX 모드에서 메인루프와 콜백이 공유)
extern volatile uint8_t  cloneRxChannelBuffer;    // 수신된 WiFi 채널
extern volatile bool     cloneRxMacReceived;       // ClonePacket(MAC+채널) 수신 여부
extern volatile uint8_t  cloneRxSettingsCount;     // 수신 완료된 설정 청크 개수
extern DeviceSettings    cloneRxSettingsBuffer[MAX_DEVICES + 1]; // 수신된 장치 설정 버퍼

void resetCloneRxState();

namespace Comm {

// [NEW] SPARE COPY용 장치 설정 청크 패킷 (TX→TX, 장치 1개씩 전달)
#pragma pack(push, 1)
struct SettingsChunkPacket {
    uint8_t        signature[4];
    uint8_t        version;
    uint8_t        packetType;    // SETTINGS_CHUNK_PACKET (0x0A)
    uint8_t        deviceIndex;   // 1~MAX_DEVICES
    uint8_t        totalDevices;  // MAX_DEVICES (20)
    DeviceSettings settings;      // 45B
    uint8_t        crc8;
}; // 총 54B — ESP-NOW 250B 제한 내
#pragma pack(pop)

static_assert(sizeof(SettingsChunkPacket) == 4+1+1+1+1+sizeof(DeviceSettings)+1, "SettingsChunkPacket size mismatch");

inline bool verifySettingsChunkPacket(const uint8_t* data, size_t len, const SettingsChunkPacket*& pkt) {
    if (len < sizeof(SettingsChunkPacket)) return false;
    pkt = reinterpret_cast<const SettingsChunkPacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version != kVersion) return false;
    if (pkt->packetType != SETTINGS_CHUNK_PACKET) return false;
    if (pkt->deviceIndex < 1 || pkt->deviceIndex > 20) return false;
    return crc8(data, sizeof(SettingsChunkPacket) - 1) == pkt->crc8;
}

inline void fillPacket(CommPacket &pkt, PacketType type, uint8_t tgtId, uint32_t txButtonPressMicros, const DeviceSettings& settings) {
    memcpy(pkt.signature, kSig, 4);
    pkt.version               = kVersion;
    pkt.packetType            = type;
    pkt.targetId              = tgtId;
    pkt.targetMachineType     = (uint8_t)settings.machineType;
    pkt.txButtonPressMicros   = txButtonPressMicros;
    pkt.txMicros              = micros();
    pkt.stepCount             = settings.stepCount;
    memcpy(pkt.steps, settings.steps, sizeof(ExecutionStep) * settings.stepCount);
    pkt.crc8 = crc8(reinterpret_cast<const uint8_t*>(&pkt), sizeof(CommPacket) - 1);
}
} // namespace Comm

#endif // ESPNOW_T_H