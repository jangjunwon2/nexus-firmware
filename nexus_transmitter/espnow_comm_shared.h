#pragma once
#ifndef ESPNOW_COMM_SHARED_H
#define ESPNOW_COMM_SHARED_H

#include <stdint.h>
#include <string.h>
#include <Arduino.h>

//---------------------------------------------------------------------
//  공용 통신 상수 및 데이터 구조
//---------------------------------------------------------------------
constexpr uint8_t MAX_EXECUTION_STEPS = 10;

#pragma pack(push, 1)
struct ExecutionStep {
    uint8_t delayMinutes;
    uint8_t delaySeconds;
    uint8_t playSeconds;
    uint8_t pwmValue; 
};
#pragma pack(pop)

static_assert(sizeof(ExecutionStep) == 4, "ExecutionStep size mismatch");

namespace Comm {

static const uint8_t HOPPING_CHANNELS[] = { 1, 6, 11 };
constexpr uint8_t NUM_HOPPING_CHANNELS = 3;
// 3초 무선 신호 유실 시 탐색 전환을 위한 타임아웃
constexpr uint32_t CHANNEL_HOVER_TIMEOUT_MS = 3000; 
constexpr uint32_t CHANNEL_SWITCH_INTERVAL_MS = 100; // 채널별 수신 대기 시간


//---------------------------------------------------------------------
//  서명 및 버전 (송신기와 일치: 0x08)
//---------------------------------------------------------------------
static constexpr uint8_t kSig[4]   = { 'M','L','A','B' };
static constexpr uint8_t kVersion  = 0x08;

//---------------------------------------------------------------------
//  패킷 레이아웃
//---------------------------------------------------------------------
#pragma pack(push, 1)

enum PacketType : uint8_t {
    RTT_REQUEST = 0x01,
    FINAL_COMMAND = 0x02,
    CLONE_MAC_ANNOUNCE = 0x03 // 송신기간 무선 복제용 패킷 타입
};

struct CommPacket {
    uint8_t  signature[4];
    uint8_t  version;
    uint8_t  packetType;
    uint8_t  targetId;
    uint8_t  targetMachineType; 
    uint32_t txButtonPressMicros;
    uint32_t txMicros;
    uint32_t lastKnownRttUs;
    uint32_t lastKnownRxProcessingTimeUs;
    uint8_t  stepCount;
    ExecutionStep steps[MAX_EXECUTION_STEPS]; 
    uint8_t  crc8;
};

struct AckPacket {
    uint8_t  signature[4];
    uint8_t  version;
    uint8_t  senderId;
    uint32_t originalTxMicros;
    uint32_t rxProcessingTimeUs;
    uint8_t  crc8;
};

// 무선 복제용 패킷 구조체
struct ClonePacket {
    uint8_t  signature[4];
    uint8_t  version;
    uint8_t  packetType;
    uint8_t  macAddress[6]; 
    uint8_t  crc8;
};

#pragma pack(pop)

// 구조체 크기 검증
static_assert(sizeof(CommPacket) == 66, "CommPacket size mismatch");
static_assert(sizeof(AckPacket) == 15, "AckPacket size mismatch");

//---------------------------------------------------------------------
//  CRC-8
//---------------------------------------------------------------------
inline uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    while (len--) {
        uint8_t inbyte = *data++;
        for (uint8_t i = 8; i; --i) {
            uint8_t mix = (crc ^ inbyte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            inbyte >>= 1;
        }
    }
    return crc;
}

//---------------------------------------------------------------------
//  수신부 헬퍼 함수
//---------------------------------------------------------------------
inline bool verifyCommPacket(const uint8_t* data, size_t len, const CommPacket*& pkt, uint8_t myId, bool &forMe) {
    if (len < sizeof(CommPacket)) return false; 
    pkt = reinterpret_cast<const CommPacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version != kVersion) return false;
    uint8_t calc_crc = crc8(data, sizeof(CommPacket) - 1);
    if (calc_crc != pkt->crc8) return false;
    
    forMe = (pkt->targetId == 0) || (pkt->targetId == myId);
    return true;
}

inline bool verifyAckPacket(const uint8_t* data, size_t len, const AckPacket*& pkt) {
    if (len < sizeof(AckPacket)) return false;
    pkt = reinterpret_cast<const AckPacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version != kVersion) return false;
    uint8_t calc_crc = crc8(data, sizeof(AckPacket) - 1);
    if (calc_crc != pkt->crc8) return false;
    return true;
}

inline void fillAckPacket(AckPacket& ack, uint8_t senderId, uint32_t originalTxMicros, uint32_t rxProcessingTime) {
    memcpy(ack.signature, kSig, 4);
    ack.version = kVersion;
    ack.senderId = senderId;
    ack.originalTxMicros = originalTxMicros;
    ack.rxProcessingTimeUs = rxProcessingTime;
    ack.crc8 = crc8(reinterpret_cast<const uint8_t*>(&ack), sizeof(AckPacket) - 1);
}

} // namespace Comm

#endif // ESPNOW_COMM_SHARED_H