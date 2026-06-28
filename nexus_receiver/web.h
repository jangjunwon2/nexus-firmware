#pragma once
#ifndef WEB_H
#define WEB_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "config.h"
#include "utils.h"
#include <WiFi.h>
#include <atomic> 

class ModeManager;
class CommManager;

class WebManager {
public:
    WebManager();
    void begin(ModeManager* modeMgr, CommManager* commMgr);
    
    void startServer();
    void stopServer();
    bool isServerRunning() const;

    void performUpdateAndReboot();
    void broadcastTestComplete();

    static WebManager* _instance;

private:
    friend class ModeManager;

    AsyncWebServer _server;
    AsyncWebSocket _ws;
    ModeManager* _modeManager;
    CommManager* _commManager;

    // [수정] std.atomic -> std::atomic 으로 네임스페이스 문법 오류 수정
    std::atomic<bool> _isServerRunning;
    std::atomic<bool> _otaUpdateDownloaded;
    std::atomic<bool> _isScanningWifi;
    std::atomic<bool> _isConnectingWifi;
    std::atomic<bool> _isCheckingOta;
    std::atomic<bool> _isDownloadingOta;

    String _currentFirmwareVersion;
    String _latestOtaVersion;
    String _otaChangeLog;
    String _otaFirmwareUrl;
    bool _otaUpdateAvailable;

    wifi_event_id_t _wifiEventId;
    uint8_t _lastDisconnectReason;
    unsigned long _wifiConnectStartMillis;

    SemaphoreHandle_t _otaDataMutex;

    String _disconnectedForTestSsid;
    bool _reconnectOnExitTest;

    volatile bool _pendingSaveCredential;
    char _pendingCredSSID[65];
    char _pendingCredPwd[65];

    void setupRoutes();
    void setupLogBroadcaster();
    void setupWebSocket();
    void reconnectWifiIfNeeded();

    void handleRoot(AsyncWebServerRequest* request);
    void handleManualPage(AsyncWebServerRequest* request);
    void handleWifiConfigPage(AsyncWebServerRequest* request);
    void handleFirmwareUpdatePage(AsyncWebServerRequest* request);
    void handleTestModePage(AsyncWebServerRequest* request);
    void handleExit(AsyncWebServerRequest* request);
    void handleNotFound(AsyncWebServerRequest* request);

    void handleScanWifiApi(AsyncWebServerRequest* request);
    void handleConnectWifiApi(AsyncWebServerRequest* request);
    void handleDisconnectWifiApi(AsyncWebServerRequest* request);
    void handleWifiStatusApi(AsyncWebServerRequest* request);
    void handleCheckOtaApi(AsyncWebServerRequest* request);
    void handleDownloadOtaApi(AsyncWebServerRequest* request);
    void handleDeviceStatusApi(AsyncWebServerRequest* request);
    void handleSetDeviceIdApi(AsyncWebServerRequest* request);
    void handleRunTestApi(AsyncWebServerRequest* request);

    void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len);
    
    static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

    static void wifiScanTask(void* pvParameters);
    static void otaCheckVersionTask(void* pvParameters);
    static void otaDownloadTask(void* pvParameters);

    bool fetchOtaVersionInfo();
    void downloadAndApplyOta();

    void getWifiStatusJson(JsonDocument& doc);
    void broadcastWifiStatus(const char* status, int reason = 0);
    void broadcastOtaStatus();
    void broadcastOtaProgress(int progress);
    void broadcastJson(const JsonDocument& doc);
    
    String getPageHeader(const String& title, const char* i18nKey = nullptr);
    String getPageFooter(bool showHomeButton);

    void loop();
};

#endif // WEB_H