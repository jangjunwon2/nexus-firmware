#ifndef ESPNOW_T_H
#define ESPNOW_T_H

#include "config_t.h"
#include "espnow_comm_shared.h"
#include <esp_now.h>
#include <WiFi.h>

extern bool executionComplete; 
extern unsigned long lastEspNowTxTime; 

bool initEspNow();
void espNowSendCb(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len);
bool manageCommunication();
bool sendExecutionCommand(Comm::PacketType type, RunningDevice& device, uint32_t& out_tx_timestamp);
void deinitEspNow();

// [NEW] 무선 복제 송신 함수 선언
void sendCloneAnnounce();

namespace Comm {
inline void fillPacket(CommPacket &pkt, PacketType type, uint8_t tgtId, uint32_t txButtonPressMicros, const DeviceSettings& settings, uint32_t rttUs, uint32_t rxProcessingTimeUs) {
    memcpy(pkt.signature, kSig, 4);
    pkt.version        = kVersion;
    pkt.packetType     = type;
    pkt.targetId       = tgtId;
    pkt.targetMachineType = (uint8_t)settings.machineType;
    pkt.txButtonPressMicros = txButtonPressMicros;
    pkt.txMicros       = micros();
    pkt.lastKnownRttUs = rttUs;
    pkt.lastKnownRxProcessingTimeUs = rxProcessingTimeUs;

    if (type == FINAL_COMMAND) {
        pkt.stepCount = settings.stepCount;
        memcpy(pkt.steps, settings.steps, sizeof(ExecutionStep) * settings.stepCount);
    } else {
        pkt.stepCount = 0;
        memset(pkt.steps, 0, sizeof(pkt.steps));
    }

    pkt.crc8 = crc8(reinterpret_cast<const uint8_t*>(&pkt), sizeof(CommPacket) - 1);
}
} // namespace Comm

#endif // ESPNOW_T_H