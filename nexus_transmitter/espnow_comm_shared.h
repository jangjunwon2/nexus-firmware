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

static constexpr uint8_t HOPPING_CHANNELS[] = { 1, 6, 11 };
constexpr uint8_t NUM_HOPPING_CHANNELS = 3;
// TX가 150ms마다 채널을 바꾸므로 300ms 이내에 반드시 겹치는 구간 확보
constexpr uint32_t CHANNEL_HOVER_TIMEOUT_MS = 300;
constexpr uint32_t CHANNEL_SWITCH_INTERVAL_MS = 1000; // 채널별 수신 대기 시간 (페어링 스캔 시 유지 시간 늘림)


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
    FIRE_COMMAND = 0x02,
    CLONE_MAC_ANNOUNCE = 0x03,
    SCAN_START_COMMAND = 0x04,
    SCAN_PROBE_PACKET = 0x05,
    SCAN_REPORT_PACKET = 0x06,
    CHANNEL_COMMIT_COMMAND = 0x07, // [NEW] 송신기→수신기 채널 확정(안전 핸드셰이크)
    HOLD_COMMAND = 0x08,           // [NEW] 홀드(누르는 동안 지속) — 킵얼라이브 방식
    CANCEL_COMMAND = 0x09,         // [NEW] 실행 취소 — 수신기 시퀀스 즉시 중단
    SETTINGS_CHUNK_PACKET = 0x0A   // [NEW] 예비 복사용 장치 설정 청크 (TX→TX)
};

struct CommPacket {
    uint8_t  signature[4];
    uint8_t  version;
    uint8_t  packetType;
    uint8_t  targetId;
    uint8_t  targetMachineType; 
    uint32_t txButtonPressMicros;
    uint32_t txMicros;
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

// 무선 복제용 패킷 구조체 (SPARE COPY: TX→TX MAC+채널 전달)
struct ClonePacket {
    uint8_t  signature[4];
    uint8_t  version;
    uint8_t  packetType;
    uint8_t  macAddress[6];
    uint8_t  wifiChannel;  // [NEW] AUTO CH로 확정된 WiFi 채널 (1/6/11)
    uint8_t  crc8;
};

// [NEW] RF 환경 분석용 스캔 시작 패킷
struct ScanStartPacket {
    uint8_t signature[4];
    uint8_t version;
    uint8_t packetType; // SCAN_START_COMMAND
    uint8_t crc8;
};

// [NEW] RF 환경 분석용 각 채널 핑 테스트 프루브 패킷
struct ScanProbePacket {
    uint8_t signature[4];
    uint8_t version;
    uint8_t packetType; // SCAN_PROBE_PACKET
    uint8_t channel;
    uint8_t probeIndex; // 0 ~ 9
    uint8_t crc8;
};

// [NEW] RF 환경 분석 최종 완료 리포트 패킷
struct ScanReportPacket {
    uint8_t signature[4];
    uint8_t version;
    uint8_t packetType; // SCAN_REPORT_PACKET
    uint8_t senderId;
    uint8_t bestChannel;
    uint8_t channelSuccessRates[3]; // Ch 1, 6, 11 측정성공 패킷 횟수 (0~10)
    uint8_t crc8;
};

// [NEW] 채널 확정 패킷 (송신기→수신기). 송신기가 리포트 수신 후 확정 채널을 통보 →
// 양쪽이 함께 전환. 안정적인 TX→RX 방향으로 전송하여 채널 불일치 방지.
struct ChannelCommitPacket {
    uint8_t signature[4];
    uint8_t version;
    uint8_t packetType; // CHANNEL_COMMIT_COMMAND
    uint8_t channel;    // 확정 채널 (1/6/11)
    uint8_t crc8;
};

// [NEW] 홀드 패킷 (송신기→수신기). 누르는 동안 50~100ms 주기로 반복 송신(킵얼라이브).
// 수신기는 이 패킷이 들어오는 동안만 출력 ON, 일정시간(~250ms) 끊기면 자동 OFF.
// 그룹 홀드는 송신기가 멤버 ID별로 순회 송신한다.
struct HoldPacket {
    uint8_t signature[4];
    uint8_t version;
    uint8_t packetType;        // HOLD_COMMAND
    uint8_t targetId;          // 대상 장치 ID
    uint8_t targetMachineType; // 타입 필터 (FIRE와 동일 규칙)
    uint8_t pwmValue;          // 출력 세기 0~100
    uint8_t holdActive;        // 1=ON 유지, 0=즉시 OFF(뗄 때 보조 송신)
    uint8_t crc8;
};

// [NEW] 실행 취소 패킷 (송신기→수신기). 수신기는 즉시 시퀀스를 중단하고 출력을 끈다.
struct CancelPacket {
    uint8_t signature[4];
    uint8_t version;
    uint8_t packetType;  // CANCEL_COMMAND
    uint8_t targetId;    // 0=전체, N=특정 기기 ID
    uint8_t crc8;
};

#pragma pack(pop)

// 구조체 크기 검증
static_assert(sizeof(CommPacket) == 58, "CommPacket size mismatch");
static_assert(sizeof(AckPacket) == 15, "AckPacket size mismatch");
static_assert(sizeof(ClonePacket) == 14, "ClonePacket size mismatch");
static_assert(sizeof(ScanStartPacket) == 7, "ScanStartPacket size mismatch");
static_assert(sizeof(ScanProbePacket) == 9, "ScanProbePacket size mismatch");
static_assert(sizeof(ScanReportPacket) == 12, "ScanReportPacket size mismatch");
static_assert(sizeof(ChannelCommitPacket) == 8, "ChannelCommitPacket size mismatch");
static_assert(sizeof(HoldPacket) == 11, "HoldPacket size mismatch");
static_assert(sizeof(CancelPacket) == 8, "CancelPacket size mismatch");

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
    if (pkt->packetType != FIRE_COMMAND) return false;
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

// [NEW] RF 스캔 패킷 검증 헬퍼 함수들
inline bool verifyScanStartPacket(const uint8_t* data, size_t len, const ScanStartPacket*& pkt) {
    if (len < sizeof(ScanStartPacket)) return false;
    pkt = reinterpret_cast<const ScanStartPacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version != kVersion) return false;
    if (pkt->packetType != SCAN_START_COMMAND) return false;
    uint8_t calc_crc = crc8(data, sizeof(ScanStartPacket) - 1);
    if (calc_crc != pkt->crc8) return false;
    return true;
}

inline bool verifyScanProbePacket(const uint8_t* data, size_t len, const ScanProbePacket*& pkt) {
    if (len < sizeof(ScanProbePacket)) return false;
    pkt = reinterpret_cast<const ScanProbePacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version != kVersion) return false;
    if (pkt->packetType != SCAN_PROBE_PACKET) return false;
    uint8_t calc_crc = crc8(data, sizeof(ScanProbePacket) - 1);
    if (calc_crc != pkt->crc8) return false;
    return true;
}

inline bool verifyScanReportPacket(const uint8_t* data, size_t len, const ScanReportPacket*& pkt) {
    if (len < sizeof(ScanReportPacket)) return false;
    pkt = reinterpret_cast<const ScanReportPacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version != kVersion) return false;
    if (pkt->packetType != SCAN_REPORT_PACKET) return false;
    uint8_t calc_crc = crc8(data, sizeof(ScanReportPacket) - 1);
    if (calc_crc != pkt->crc8) return false;
    return true;
}

inline bool verifyChannelCommitPacket(const uint8_t* data, size_t len, const ChannelCommitPacket*& pkt) {
    if (len < sizeof(ChannelCommitPacket)) return false;
    pkt = reinterpret_cast<const ChannelCommitPacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version != kVersion) return false;
    if (pkt->packetType != CHANNEL_COMMIT_COMMAND) return false;
    uint8_t calc_crc = crc8(data, sizeof(ChannelCommitPacket) - 1);
    if (calc_crc != pkt->crc8) return false;
    return true;
}

inline void fillChannelCommitPacket(ChannelCommitPacket& pkt, uint8_t channel) {
    memcpy(pkt.signature, kSig, 4);
    pkt.version = kVersion;
    pkt.packetType = CHANNEL_COMMIT_COMMAND;
    pkt.channel = channel;
    pkt.crc8 = crc8(reinterpret_cast<const uint8_t*>(&pkt), sizeof(ChannelCommitPacket) - 1);
}

inline bool verifyHoldPacket(const uint8_t* data, size_t len, const HoldPacket*& pkt) {
    if (len < sizeof(HoldPacket)) return false;
    pkt = reinterpret_cast<const HoldPacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version != kVersion) return false;
    if (pkt->packetType != HOLD_COMMAND) return false;
    uint8_t calc_crc = crc8(data, sizeof(HoldPacket) - 1);
    if (calc_crc != pkt->crc8) return false;
    return true;
}

inline void fillCancelPacket(CancelPacket& pkt, uint8_t targetId) {
    memcpy(pkt.signature, kSig, 4);
    pkt.version    = kVersion;
    pkt.packetType = CANCEL_COMMAND;
    pkt.targetId   = targetId;
    pkt.crc8       = crc8(reinterpret_cast<const uint8_t*>(&pkt), sizeof(CancelPacket) - 1);
}

inline bool verifyCancelPacket(const uint8_t* data, size_t len, const CancelPacket*& pkt) {
    if (len < sizeof(CancelPacket)) return false;
    pkt = reinterpret_cast<const CancelPacket*>(data);
    if (memcmp(pkt->signature, kSig, 4) != 0) return false;
    if (pkt->version    != kVersion)       return false;
    if (pkt->packetType != CANCEL_COMMAND) return false;
    return crc8(data, sizeof(CancelPacket) - 1) == pkt->crc8;
}

inline void fillHoldPacket(HoldPacket& pkt, uint8_t targetId, uint8_t machineType, uint8_t pwm, uint8_t active) {
    memcpy(pkt.signature, kSig, 4);
    pkt.version = kVersion;
    pkt.packetType = HOLD_COMMAND;
    pkt.targetId = targetId;
    pkt.targetMachineType = machineType;
    pkt.pwmValue = pwm;
    pkt.holdActive = active;
    pkt.crc8 = crc8(reinterpret_cast<const uint8_t*>(&pkt), sizeof(HoldPacket) - 1);
}

} // namespace Comm

#endif // ESPNOW_COMM_SHARED_H