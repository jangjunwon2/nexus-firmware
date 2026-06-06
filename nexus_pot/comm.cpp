#include "comm.h"
#include "mode.h"
#include "utils.h"
#include <esp_wifi.h>

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

    Log::Info(PSTR("COMM: Initializing ESP-NOW (Receiver, ID: %d)"), _myDeviceId);

    _hasMasterMac = Utils::loadMasterMac(_masterMac);
    if (_hasMasterMac) {
        Log::Info(PSTR("COMM: Master MAC Loaded: %02X:%02X:%02X:%02X:%02X:%02X"), 
            _masterMac[0], _masterMac[1], _masterMac[2], _masterMac[3], _masterMac[4], _masterMac[5]);
    } else {
        Log::Warn(PSTR("COMM: No Master MAC registered yet. Please pair with a transmitter."));
    }

    WiFi.mode(WIFI_STA);

    if (!initEspNow()) {
        return false;
    }

    _lastPacketRecvTime = millis(); // 시작 시점 유실 타이머 구동
    xTaskCreatePinnedToCore(channelHoppingTask, "HoppingTask", 3072, this, 2, NULL, 0);

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
    Log::Info(PSTR("COMM: Master Transmitter Paired Successfully!"));
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
    
    if (_isPairingMode && len == sizeof(Comm::ClonePacket)) {
        const Comm::ClonePacket* clonePkt = reinterpret_cast<const Comm::ClonePacket*>(incomingData);
        if (memcmp(clonePkt->signature, Comm::kSig, 4) == 0 && clonePkt->packetType == Comm::CLONE_MAC_ANNOUNCE) {
            uint8_t calculated_crc = Comm::crc8(incomingData, sizeof(Comm::ClonePacket) - 1);
            if (calculated_crc == clonePkt->crc8) {
                registerMasterMac(recv_info->src_addr);
            }
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

    Log::Info(PSTR("COMM: Valid packet received. Passing to ModeManager."));
    
    if (_modeManager) {
        _modeManager->handleEspNowCommand(recv_info->src_addr, *pkt, rxTime);
    }
}

void CommManager::handleEspNowSendStatus(esp_now_send_status_t status) {}

void CommManager::sendAck(const uint8_t* targetMac, const Comm::CommPacket& originalPacket, uint32_t rx_time) {
    Comm::AckPacket ackPacket;
    uint32_t rxProcessingTime = micros() - rx_time;
    Comm::fillAckPacket(ackPacket, _myDeviceId, originalPacket.txMicros, rxProcessingTime);
    
    if (!esp_now_is_peer_exist(targetMac)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, targetMac, 6);
        peer.channel = 0; // 동적 채널 전송 허용
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK && esp_now_add_peer(&peer) != ESP_ERR_ESPNOW_EXIST) return;
    }
    esp_now_send(targetMac, (uint8_t*)&ackPacket, sizeof(ackPacket));
}

uint8_t CommManager::getChannel() const { 
    return ESP_NOW_CHANNEL; 
}

void CommManager::channelHoppingTask(void* param) {
    CommManager* self = static_cast<CommManager*>(param);
    uint8_t hopIdx = 0;
    
    for (;;) {
        // ModeManager가 있고 NORMAL 또는 PAIRING 모드일 때만 채널 호핑 스캔을 돌림
        // (WIFI/TEST 모드 시 SoftAP 채널 유지를 위해 정지)
        if (self->_modeManager && 
           (self->_modeManager->getCurrentMode() == DeviceMode::MODE_NORMAL || 
            self->_modeManager->getCurrentMode() == DeviceMode::MODE_PAIRING)) {
            
            unsigned long currentTime = millis();
            if (currentTime - self->_lastPacketRecvTime > Comm::CHANNEL_HOVER_TIMEOUT_MS) {
                // 신호 유실: 다음 채널로 전환하며 스캔
                hopIdx = (hopIdx + 1) % Comm::NUM_HOPPING_CHANNELS;
                uint8_t targetChannel = Comm::HOPPING_CHANNELS[hopIdx];
                
                if (self->_currentHopChannel != targetChannel) {
                    self->_currentHopChannel = targetChannel;
                    esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
                    Log::Debug(PSTR("COMM: Channel lost. Hopping to Ch %d"), targetChannel);
                }
                vTaskDelay(pdMS_TO_TICKS(Comm::CHANNEL_SWITCH_INTERVAL_MS));
                continue;
            }
        } else {
            // WIFI 또는 다른 모드일 때는 기본 채널로 설정 고정
            if (self->_currentHopChannel != ESP_NOW_CHANNEL) {
                self->_currentHopChannel = ESP_NOW_CHANNEL;
                esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
