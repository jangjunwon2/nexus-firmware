#pragma once
#ifndef COMM_H
#define COMM_H

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi_types.h> 
#include "config.h"
#include "espnow_comm_shared.h"

class ModeManager;

extern "C" void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* incomingData, int len);
extern "C" void onDataSent(const esp_now_send_info_t* info, esp_now_send_status_t status);

class CommManager {
public:
    CommManager();
    bool begin(uint8_t deviceId, ModeManager* modeMgr);
    void updateMyDeviceId(uint8_t newId);
    
    void handleEspNowRecv(const esp_now_recv_info_t* recv_info, const uint8_t* incomingData, int len);
    void handleEspNowSendStatus(esp_now_send_status_t status);
    
    void sendAck(const uint8_t* targetMac, const Comm::CommPacket& originalPacket, uint32_t rx_time);
    void reinitForEspNow();
    void prepareForWifiMode();
    uint8_t getChannel() const;

    void setPairingMode(bool isPairing);

    static CommManager* _instance;

private:
    ModeManager* _modeManager;
    uint8_t _myDeviceId;

    uint8_t _masterMac[6];
    bool _hasMasterMac;
    bool _isPairingMode;

    void registerMasterMac(const uint8_t* mac);
    bool initEspNow();
    void registerCallbacks();

    unsigned long _lastPacketRecvTime;
    uint8_t _currentHopChannel;
    static void channelHoppingTask(void* param);
};

#endif // COMM_H