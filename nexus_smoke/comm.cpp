#include "comm.h"
#include "mode.h"
#include "utils.h"
#include <esp_wifi.h>

uint8_t ESP_NOW_CHANNEL = 1;
CommManager* CommManager::_instance = nullptr;

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* incomingData, int len) {
    if (CommManager::_instance) {
        CommManager::_instance->handleEspNowRecv(info, incomingData, len);
    }
}

void onDataSent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
    if (CommManager::_instance) {
        CommManager::_instance->handleEspNowSendStatus(status);
    }
}

CommManager::CommManager() : 
    _modeManager(nullptr), 
    _myDeviceId(DEFAULT_DEVICE_ID), 
    _hasMasterMac(false), 
    _isPairingMode(false),
    _lastPacketRecvTime(0),
    _currentHopChannel(ESP_NOW_CHANNEL)
{
    memset(_masterMac, 0, 6);
}

bool CommManager::begin(uint8_t deviceId, ModeManager* modeMgr) {
    _instance = this;
    _myDeviceId = deviceId;
    _modeManager = modeMgr;

    // [AUTO CH] 저장된 최적 채널 로드 (AUTO CH 스캔 결과, 기본/검증실패 시 Ch 1).
    // 페어링·AUTO CH 핸드셰이크는 항상 Ch 1 랑데부 → 채널 어긋나도 AUTO CH로 자가복구.
    uint8_t savedCh = Utils::loadCommChannel();
    ESP_NOW_CHANNEL = (savedCh == 1 || savedCh == 6 || savedCh == 11) ? savedCh : 1;
    _currentHopChannel = ESP_NOW_CHANNEL;

    Log::Info(PSTR("COMM: Initializing ESP-NOW (Receiver, ID: %d, Channel: %d)"), _myDeviceId, ESP_NOW_CHANNEL);

    _hasMasterMac = Utils::loadMasterMac(_masterMac);
    if (_hasMasterMac) {
        Log::Info(PSTR("COMM: Master MAC Loaded: %02X:%02X:%02X:%02X:%02X:%02X"), 
            _masterMac[0], _masterMac[1], _masterMac[2], _masterMac[3], _masterMac[4], _masterMac[5]);
    } else {
        Log::Warn(PSTR("COMM: No Master MAC registered yet. Please pair with a transmitter."));
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    // 절전 비활성화는 initEspNow()에서 처리 (모든 init 경로 공통)

    if (!initEspNow()) {
        return false;
    }

    _lastPacketRecvTime = millis(); // 시작 시점 유실 타이머 구동
    xTaskCreatePinnedToCore(channelHoppingTask, "HoppingTask", 4096, this, 2, NULL, 0);

    Log::Info(PSTR("COMM: ESP-NOW initialized successfully (Channel: %d)."), ESP_NOW_CHANNEL);
    return true;
}

void CommManager::setPairingMode(bool isPairing) {
    _isPairingMode = isPairing;
}

void CommManager::registerMasterMac(const uint8_t* mac) {
    memcpy(_masterMac, mac, 6);
    _hasMasterMac = true;
    Utils::saveMasterMac(mac);
    // [복구] 페어링 = 알려진 기본 채널(Ch1)로 복귀. 송신기도 페어링 시 Ch1로 리셋하므로
    // 페어링만 하면 두 기기가 항상 같은 채널(Ch1)에 모임 → 채널 불일치 상태에서 탈출.
    ESP_NOW_CHANNEL = 1;
    Utils::saveCommChannel(1);
    Log::Info(PSTR("COMM: Master Transmitter Paired Successfully! (Channel reset to 1)"));
    if (_modeManager) {
        _modeManager->notifyPairingSuccess();
    }
}

void CommManager::reinitForEspNow() {
    Log::Info(PSTR("COMM: Reinitializing ESP-NOW..."));
    if (WiFi.isConnected()) {
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_now_deinit();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    if (!initEspNow()) {
        Log::Error(PSTR("COMM: Failed to reinitialize ESP-NOW."));
    } else {
        Log::Info(PSTR("COMM: ESP-NOW reinitialized successfully."));
    }
}

bool CommManager::initEspNow() {
    if (esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    if (esp_now_init() != ESP_OK) {
        if (_modeManager) _modeManager->switchToMode(DeviceMode::MODE_ERROR, true);
        return false;
    }

    // [CRITICAL] 절전 비활성화는 esp_now_init(=WiFi 완전 시작) 이후에 호출해야 확실히 적용됨.
    // 너무 일찍 호출하면 부팅 직후 적용 실패 → 간헐적 수신 불능.
    esp_wifi_set_ps(WIFI_PS_NONE);
    // [발열/전류] 근거리엔 최대출력 과함 → 13dBm. 전류 스파이크/발열 감소, 신호는 충분히 강함.
    esp_wifi_set_max_tx_power(52); // 52 = 13dBm

    registerCallbacks();
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, BROADCAST_ADDRESS, 6);
    peerInfo.channel = 0; // 동적 와이파이 채널 추적 허용
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) return false;
    
    return true;
}

void CommManager::registerCallbacks() {
    if (esp_now_register_recv_cb(onDataRecv) != ESP_OK) Log::Error(PSTR("COMM: 수신 콜백 등록 실패."));
    if (esp_now_register_send_cb(reinterpret_cast<esp_now_send_cb_t>(onDataSent)) != ESP_OK) Log::Error(PSTR("COMM: 송신 콜백 등록 실패."));
}

void CommManager::updateMyDeviceId(uint8_t newId) {
    if (_myDeviceId != newId) {
        Log::Info(PSTR("COMM: Device ID updated from %d to %d (Receiver)."), _myDeviceId, newId);
        _myDeviceId = newId;
    }
}

void CommManager::handleEspNowRecv(const esp_now_recv_info_t* recv_info, const uint8_t* incomingData, int len) {
    uint32_t rxTime = micros(); 
    
    // [DEBUG] 수신 패킷 로깅. 단, 대량 유입되는 것들은 콜백 차단(→ ACK 지연/드롭) 방지를 위해 생략:
    // FIRE(0x02)·프루브(0x05) 항상 생략, 클론(0x03)은 페어링 중이 아닐 때만 생략(송신기 자동반복 스팸).
    if (len >= 6) {
        uint8_t pType = incomingData[5];
        bool suppress = (pType == Comm::SCAN_PROBE_PACKET) || (pType == Comm::FIRE_COMMAND) ||
                        (pType == Comm::CLONE_MAC_ANNOUNCE && !_isPairingMode);
        if (!suppress) {
            Log::Info(PSTR("COMM: Recv len: %d, type: 0x%02X, pairingMode: %d"), len, pType, _isPairingMode);
        }
    }
    
    if (_isPairingMode) {
        if (len == sizeof(Comm::ClonePacket)) {
            const Comm::ClonePacket* clonePkt = reinterpret_cast<const Comm::ClonePacket*>(incomingData);
            if (clonePkt->packetType == Comm::CLONE_MAC_ANNOUNCE) {
                if (memcmp(clonePkt->signature, Comm::kSig, 4) == 0) {
                    uint8_t calculated_crc = Comm::crc8(incomingData, sizeof(Comm::ClonePacket) - 1);
                    if (calculated_crc == clonePkt->crc8) {
                        Log::Info(PSTR("COMM: Valid ClonePacket received. Registering Master MAC..."));
                        registerMasterMac(recv_info->src_addr);
                    } else {
                        Log::Warn(PSTR("COMM: ClonePacket CRC mismatch (calc: 0x%02X, recv: 0x%02X)"), calculated_crc, clonePkt->crc8);
                    }
                } else {
                    Log::Warn(PSTR("COMM: ClonePacket signature invalid"));
                }
            } else {
                Log::Warn(PSTR("COMM: ClonePacket type invalid (0x%02X)"), clonePkt->packetType);
            }
        } else {
            Log::Debug(PSTR("COMM: Recv len %d does not match ClonePacket size (%d) during pairing"), len, sizeof(Comm::ClonePacket));
        }
        return; 
    }

    // [NEW] 채널 확정(COMMIT) 패킷 수신 감지 (AUTO CH 안전 핸드셰이크)
    const Comm::ChannelCommitPacket* commitPkt = nullptr;
    if (Comm::verifyChannelCommitPacket(incomingData, len, commitPkt)) {
        Log::Info(PSTR("COMM: Channel commit received -> Ch %d"), commitPkt->channel);
        if (_modeManager) _modeManager->handleChannelCommit(commitPkt->channel);
        return;
    }

    // [NEW] RF 스캔 개시 패킷 수신 감지
    const Comm::ScanStartPacket* startPkt = nullptr;
    if (Comm::verifyScanStartPacket(incomingData, len, startPkt)) {
        Log::Info(PSTR("COMM: Scan start packet detected."));
        if (_modeManager) _modeManager->handleScanStartCommand();
        return;
    }

    // [NEW] RF 스캔 프루브 패킷 수신 감지
    const Comm::ScanProbePacket* probePkt = nullptr;
    if (Comm::verifyScanProbePacket(incomingData, len, probePkt)) {
        if (_modeManager) _modeManager->handleScanProbePacket(probePkt->channel, probePkt->probeIndex);
        return;
    }

    // [NEW] 취소 패킷 수신 감지 (송신기 B 버튼 길게 누름 → 즉시 시퀀스 중단)
    const Comm::CancelPacket* cancelPkt = nullptr;
    if (Comm::verifyCancelPacket(incomingData, len, cancelPkt)) {
        Log::Info(PSTR("COMM: CANCEL received — targetId=%d, myId=%d"), cancelPkt->targetId, _myDeviceId);
        if (cancelPkt->targetId == 0 || cancelPkt->targetId == _myDeviceId) {
            if (_modeManager) _modeManager->cancelPlaySequence();
        }
        return;
    }

    // [NEW] 홀드 패킷 수신 감지 (송신기 PLAY 버튼 누르는 동안 지속 출력)
    const Comm::HoldPacket* holdPkt = nullptr;
    if (Comm::verifyHoldPacket(incomingData, len, holdPkt)) {
        Log::Info(PSTR("COMM: HOLD verify OK -> targetId=%d myId=%d active=%d pwm=%d"),
                  holdPkt->targetId, _myDeviceId, holdPkt->holdActive, holdPkt->pwmValue);
        if (holdPkt->targetId == _myDeviceId || holdPkt->targetId == 0) {
            if (_modeManager) _modeManager->handleHold(holdPkt->pwmValue, holdPkt->holdActive);
        } else {
            Log::Warn(PSTR("COMM: HOLD ignored — targetId=%d != myId=%d"), holdPkt->targetId, _myDeviceId);
        }
        return;
    }

    const Comm::CommPacket* pkt = nullptr;
    bool forMe = false;
    
    if (!Comm::verifyCommPacket(incomingData, len, pkt, _myDeviceId, forMe)) return;
    if (!forMe) return;

    // 유효 패킷 수신 시 채널 고정을 위해 리시브 타임아웃 리셋
    _lastPacketRecvTime = millis();

    if (_hasMasterMac) {
        if (memcmp(recv_info->src_addr, _masterMac, 6) != 0) {
            Log::Warn(PSTR("COMM: Packet from Unregistered Transmitter MAC! Ignored."));
            return;
        }
    }

    // [DEBUG] FIRE 수신 신호세기 (300ms마다 1회) — TX→RX 링크 품질 진단.
    // RSSI가 -75dBm보다 낮거나 크게 출렁이면 거리/안테나/전원 불안정 의심.
    static unsigned long lastRssiLog = 0;
    if (millis() - lastRssiLog > 300) {
        lastRssiLog = millis();
        int rssi = (recv_info && recv_info->rx_ctrl) ? recv_info->rx_ctrl->rssi : 0;
        Log::Info(PSTR("COMM: FIRE RX RSSI: %d dBm"), rssi);
    }

    if (_modeManager) {
        _modeManager->handleEspNowCommand(recv_info->src_addr, *pkt, rxTime);
    }
}

void CommManager::handleEspNowSendStatus(esp_now_send_status_t status) {}

void CommManager::sendAck(const uint8_t* targetMac, const Comm::CommPacket& originalPacket, uint32_t rx_time) {
    Comm::AckPacket ackPacket;
    uint32_t rxProcessingTime = micros() - rx_time;
    Comm::fillAckPacket(ackPacket, _myDeviceId, originalPacket.txMicros, rxProcessingTime);

    // ACK는 BROADCAST로 전송 (유니캐스트 링크레이어 ACK 의존성 제거).
    // 무선 링크가 유실이 잦으므로 3회 송출로 도달률 향상. 수신기측 txMicros 중복제거(_lastAckedTxMicros)로
    // FIRE 사본마다가 아니라 '고유 발사당' 3회만 나가므로 과거의 ACK 폭주는 발생하지 않음.
    for (int i = 0; i < 3; i++) {
        esp_now_send(BROADCAST_ADDRESS, (uint8_t*)&ackPacket, sizeof(ackPacket));
    }
}

// [NEW] 지연 ACK 송출 — 송신기 FIRE 버스트가 끝난 뒤(반이중 충돌 회피) 메인루프에서 호출됨.
void CommManager::sendAckBroadcast(uint32_t originalTxMicros) {
    Comm::AckPacket ackPacket;
    Comm::fillAckPacket(ackPacket, _myDeviceId, originalTxMicros, 0);
    for (int i = 0; i < 3; i++) {
        esp_now_send(BROADCAST_ADDRESS, (uint8_t*)&ackPacket, sizeof(ackPacket));
    }
}

uint8_t CommManager::getChannel() const {
    return ESP_NOW_CHANNEL;
}

void CommManager::channelHoppingTask(void* param) {
    CommManager* self = static_cast<CommManager*>(param);
    bool wasInSpecialMode = false;
    
    for (;;) {
        uint8_t targetChannel = ESP_NOW_CHANNEL;
        if (self->_modeManager) {
            DeviceMode m = self->_modeManager->getCurrentMode();
            if (m == DeviceMode::MODE_PAIRING || m == DeviceMode::MODE_RF_SCAN || m == DeviceMode::MODE_WIFI || m == DeviceMode::MODE_EXIT_WIFI) {
                // 페어링, 스캔, Wi-Fi 모드일 때는 호핑 태스크가 채널을 건드리지 않고
                // 시스템 및 웹/OTA 기능이 지정한 채널을 그대로 유지하게 함
                wasInSpecialMode = true;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }
        
        // 특수 모드를 방금 이탈한 경우 캐시 강제 무효화하여 물리 채널 즉시 갱신 유도
        if (wasInSpecialMode) {
            self->_currentHopChannel = 0;
            wasInSpecialMode = false;
        }
        
        // NORMAL 또는 다른 모드일 때는 기본 채널로 설정 고정
        if (self->_currentHopChannel != targetChannel) {
            self->_currentHopChannel = targetChannel;
            esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
            Log::Debug(PSTR("COMM: Normal mode. Channel fixed to Ch %d"), targetChannel);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}