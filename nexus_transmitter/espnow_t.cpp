// transmitter_s3/espnow_t.cpp
#include "espnow_t.h"
#include "utils_t.h"
#include <algorithm>
#include <esp_mac.h>

void espNowSendCb(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    lastEspNowTxTime = millis();
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    // [NEW] 복제 수신 대기 모드일 때 가로채기
    if (currentMode == MODE_CLONE_RX && len == sizeof(Comm::ClonePacket)) {
        const Comm::ClonePacket* clonePkt = (const Comm::ClonePacket*)data;
        if (memcmp(clonePkt->signature, Comm::kSig, 4) == 0 && clonePkt->packetType == Comm::CLONE_MAC_ANNOUNCE) {
            uint8_t calc_crc = Comm::crc8(data, sizeof(Comm::ClonePacket) - 1);
            if (calc_crc == clonePkt->crc8) {
                for (int i = 0; i < 6; i++) {
                    EEPROM.write(EEPROM_CLONED_MAC_ADDR + i, clonePkt->macAddress[i]);
                }
                EEPROM.write(EEPROM_CLONED_MAC_FLAG, 0xAA);
                EEPROM.commit();
                
                logPrintf(LogLevel::LOG_INFO, "CLONE: SUCCESS! Rebooting as Clone...");
                delay(500);
                ESP.restart(); // 쌍둥이로 변신하기 위해 자동 재부팅
            }
        }
        return; 
    }

    // 기존 ACK 처리
    const Comm::AckPacket* ackPkt = nullptr;
    if (!Comm::verifyAckPacket(data, len, ackPkt)) return;

    uint8_t ackingDeviceID = ackPkt->senderId;
    unsigned long rtt = micros() - ackPkt->originalTxMicros;

    for (int i = 0; i < groupDeviceCount; ++i) {
        RunningDevice& device = runningDevices[i];
        if (device.deviceID == ackingDeviceID) {
            if (device.commStatus == COMM_AWAITING_RTT_ACK) {
                if (device.lastTxTimestamp == ackPkt->originalTxMicros) {
                    device.currentSequenceRttUs = rtt;
                    device.currentSequenceRxProcessingTimeUs = ackPkt->rxProcessingTimeUs;
                    device.successfulAcks++;
                    device.commStatus = COMM_PENDING_FINAL_COMMAND;
                }
            } else if (device.commStatus == COMM_AWAITING_FINAL_ACK) {
                if (device.lastTxTimestamp == ackPkt->originalTxMicros) {
                    device.successfulAcks++;
                    device.commStatus = COMM_ACK_RECEIVED_SUCCESS;
                }
            }
            return;
        }
    }
}

bool initEspNow() {
    if (espNowInitialized) return true;
    WiFi.mode(WIFI_STA);
    delay(50);
    if (esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_send_cb(reinterpret_cast<esp_now_send_cb_t>(espNowSendCb));
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0; // 0으로 지정하여 동적 채널 변경을 완벽하게 추적
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) return false;
    espNowInitialized = true;
    return true;
}

bool sendExecutionCommand(Comm::PacketType type, RunningDevice& device, uint32_t& out_tx_timestamp) {
    Comm::CommPacket packet;
    const DeviceSettings& settings = deviceSettings[device.deviceID];
    Comm::fillPacket(packet, type, device.deviceID, device.txButtonPressSequenceMicros,
                     settings,
                     (type == Comm::FINAL_COMMAND) ? device.currentSequenceRttUs : 0,
                     (type == Comm::FINAL_COMMAND) ? device.currentSequenceRxProcessingTimeUs : 0);
    out_tx_timestamp = packet.txMicros;
    return (esp_now_send(broadcastAddress, (uint8_t*)&packet, sizeof(packet)) == ESP_OK);
}

bool manageCommunication() {
    unsigned long currentTime = millis();
    bool all_comm_done = true;
    RunningDevice* currentDeviceToProcess = nullptr;

    for (int i = 0; i < groupDeviceCount; ++i) {
        if (runningDevices[i].commStatus != COMM_ACK_RECEIVED_SUCCESS &&
            runningDevices[i].commStatus != COMM_FAILED_NO_ACK) {
            currentDeviceToProcess = &runningDevices[i];
            all_comm_done = false;
            break;
        }
    }

    if (all_comm_done) {
        // 모든 통신이 완료되었을 때 기본 채널(WIFI_CHANNEL = 1)로 복귀시킵니다.
        esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
        return true;
    }
    if (!currentDeviceToProcess) return false;

    RunningDevice& device = *currentDeviceToProcess;

    switch (device.commStatus) {
        case COMM_PENDING_RTT_REQUEST:
        case COMM_AWAITING_RTT_ACK: {
            if (device.commStatus == COMM_PENDING_RTT_REQUEST || currentTime - device.lastPacketSendTime >= RETRY_INTERVAL_MS) {
                uint32_t tx_time;
                // 전송 시도 횟수를 3으로 나누어, 3번 시도할 때까지 동일 채널(1 -> 6 -> 11) 유지
                uint8_t targetChannel = Comm::HOPPING_CHANNELS[(device.sendAttempts / 3) % Comm::NUM_HOPPING_CHANNELS];
                esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
                
                if (sendExecutionCommand(Comm::RTT_REQUEST, device, tx_time)) {
                    device.sendAttempts++;
                    device.lastPacketSendTime = currentTime;
                    device.ackTimeoutDeadline = currentTime + ACK_TIMEOUT_MS;
                    device.lastTxTimestamp = tx_time;
                    device.commStatus = COMM_AWAITING_RTT_ACK;
                }
            } else if (currentTime > device.ackTimeoutDeadline) {
                if (device.sendAttempts >= MAX_SEND_ATTEMPTS) {
                    device.commStatus = COMM_FAILED_NO_ACK;
                } else {
                    device.commStatus = COMM_PENDING_RTT_REQUEST;
                }
            }
            return false;
        }

        case COMM_PENDING_FINAL_COMMAND:
        case COMM_AWAITING_FINAL_ACK: {
            if (device.commStatus == COMM_PENDING_FINAL_COMMAND || currentTime - device.lastPacketSendTime >= RETRY_INTERVAL_MS) {
                uint32_t tx_time;
                // RTT 수신이 완료된 시점의 채널(성공 채널)을 그대로 잠금 고정하여 최종 명령 송출
                uint8_t targetChannel = Comm::HOPPING_CHANNELS[((device.sendAttempts - 1) / 3) % Comm::NUM_HOPPING_CHANNELS];
                esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);

                if (sendExecutionCommand(Comm::FINAL_COMMAND, device, tx_time)) {
                    device.sendAttempts++;
                    device.lastPacketSendTime = currentTime;
                    device.ackTimeoutDeadline = currentTime + ACK_TIMEOUT_MS;
                    device.lastTxTimestamp = tx_time;
                    device.commStatus = COMM_AWAITING_FINAL_ACK;
                    device.txButtonPressSequenceMicros = tx_time;
                }
            } else if (currentTime > device.ackTimeoutDeadline) {
                if (device.sendAttempts >= MAX_SEND_ATTEMPTS) {
                    device.commStatus = COMM_FAILED_NO_ACK;
                } else {
                    device.commStatus = COMM_PENDING_FINAL_COMMAND;
                }
            }
            return false;
        }
        default: break;
    }
    return false;
}

void deinitEspNow() {
    if (espNowInitialized) {
        esp_now_deinit();
        espNowInitialized = false;
        esp_wifi_set_channel(0, WIFI_SECOND_CHAN_NONE);
        WiFi.mode(WIFI_MODE_NULL);
    }
}

// [NEW] 무선 복제 (MAC) 신호 송출 (성공률 극대화 버스트 전송 적용)
void sendCloneAnnounce() {
    Comm::ClonePacket pkt;
    memcpy(pkt.signature, Comm::kSig, 4);
    pkt.version = Comm::kVersion;
    pkt.packetType = Comm::CLONE_MAC_ANNOUNCE;
    
    esp_read_mac(pkt.macAddress, ESP_MAC_WIFI_STA);
    pkt.crc8 = Comm::crc8((const uint8_t*)&pkt, sizeof(Comm::ClonePacket) - 1);

    // 모든 호핑 채널에 순차적으로 채널을 변경하며 버스트 전송하여, 스캔 중인 수신기가 무조건 받도록 처리
    for (uint8_t ch : Comm::HOPPING_CHANNELS) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        for (int i = 0; i < 5; i++) {
            esp_now_send(broadcastAddress, (uint8_t*)&pkt, sizeof(pkt));
            delay(15); 
        }
    }
    // 기본 채널로 복귀
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    
    logPrintf(LogLevel::LOG_INFO, "CLONE: Announce Packet Sent (All Channels Burst Mode)!");
}