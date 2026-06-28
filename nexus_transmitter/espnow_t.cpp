// transmitter_s3/espnow_t.cpp
#include "espnow_t.h"
#include "utils_t.h"
#include <esp_mac.h>
#include <esp_wifi.h>

uint8_t WIFI_CHANNEL = 1;
volatile bool cloneReceivedFlag = false;
volatile uint8_t clonedMacBuffer[6] = {};

// [NEW] RF 자동 환경 분석 관련 전역 변수 정의
volatile bool rfScanComplete = false;
static portMUX_TYPE scanMux = portMUX_INITIALIZER_UNLOCKED; // scan result 그룹 쓰기/읽기 spinlock
volatile uint8_t rfScanBestChannel = 1;
volatile uint8_t rfScanChannelRates[3] = {0, 0, 0};
volatile RfScanStatus rfScanStatus = RF_SCAN_IDLE;
static TaskHandle_t rfScanTaskHandle = NULL;
volatile bool rfScanRunning = false;

void espNowSendCb(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    lastEspNowTxTime = millis();
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!espNowInitialized) return; // deinit/reinit 진행 중 — 처리 중단
    // AUTO CH(채널 최적화) 진행 중 수신기 리포트 패킷 수신
    if (currentMode == MODE_RF_SCAN) {
        if (len == sizeof(Comm::ScanReportPacket)) {
            const Comm::ScanReportPacket* report = reinterpret_cast<const Comm::ScanReportPacket*>(data);
            if (memcmp(report->signature, Comm::kSig, 4) == 0 && report->packetType == Comm::SCAN_REPORT_PACKET) {
                uint8_t calc_crc = Comm::crc8(data, sizeof(Comm::ScanReportPacket) - 1);
                if (calc_crc == report->crc8) {
                    taskENTER_CRITICAL(&scanMux);
                    bool firstReport = !rfScanComplete;
                    rfScanBestChannel = report->bestChannel;
                    for (int i = 0; i < 3; i++) rfScanChannelRates[i] = report->channelSuccessRates[i];
                    rfScanComplete = true;
                    rfScanStatus = RF_SCAN_SUCCESS;
                    taskEXIT_CRITICAL(&scanMux);
                    if (firstReport) {
                        logPrintf(LogLevel::LOG_INFO, "RF SCAN TX: Report received. Best Ch: %d (Ch1=%d, Ch6=%d, Ch11=%d)",
                                  rfScanBestChannel, rfScanChannelRates[0], rfScanChannelRates[1], rfScanChannelRates[2]);
                    }
                }
            }
        }
        return; // 스캔 중에는 리포트만 처리
    }

    // 복제 수신 대기 모드일 때 가로채기
    if (currentMode == MODE_CLONE_RX && len == sizeof(Comm::ClonePacket)) {
        const Comm::ClonePacket* clonePkt = (const Comm::ClonePacket*)data;
        if (memcmp(clonePkt->signature, Comm::kSig, 4) == 0 && clonePkt->packetType == Comm::CLONE_MAC_ANNOUNCE) {
            uint8_t calc_crc = Comm::crc8(data, sizeof(Comm::ClonePacket) - 1);
            if (calc_crc == clonePkt->crc8) {
                for (int i = 0; i < 6; i++) clonedMacBuffer[i] = clonePkt->macAddress[i];
                cloneReceivedFlag = true;
                logPrintf(LogLevel::LOG_INFO, "CLONE RX: Valid Mac Announce from: %02X:%02X:%02X:%02X:%02X:%02X",
                          clonedMacBuffer[0], clonedMacBuffer[1], clonedMacBuffer[2], clonedMacBuffer[3], clonedMacBuffer[4], clonedMacBuffer[5]);
            } else {
                logPrintf(LogLevel::LOG_WARN, "CLONE RX: CRC mismatch (calc: 0x%02X, recv: 0x%02X).", calc_crc, clonePkt->crc8);
            }
        } else {
            logPrintf(LogLevel::LOG_WARN, "CLONE RX: Invalid packet signature or type.");
        }
        return;
    }

    // FIRE_COMMAND ACK 처리
    const Comm::AckPacket* ackPkt = nullptr;
    if (!Comm::verifyAckPacket(data, len, ackPkt)) {
        // 단, 리포트 패킷이나 다른 패킷이 일반 모드에서 들어왔을 때 오인 경보 방지
        if (len >= 6 && data[5] == Comm::SCAN_REPORT_PACKET) {
            logPrintf(LogLevel::LOG_INFO, "COMM RX: RF scan report arrived outside scan mode.");
        }
        return;
    }

    // [성능] ACK는 짧은 시간에 다수 도착할 수 있으므로 콜백 내 로깅을 최소화.
    // [DEBUG] 매칭 성공 시 RF 진단(신호세기/지연/시도횟수) 1줄 — 링크 품질 파악용.
    taskENTER_CRITICAL(&rdMux);
    int devCount = groupDeviceCount;
    taskEXIT_CRITICAL(&rdMux);
    for (int i = 0; i < devCount; ++i) {
        RunningDevice& device = runningDevices[i];
        if (device.deviceID == ackPkt->senderId) {
            taskENTER_CRITICAL(&rdMux);
            bool accepted = (device.commStatus == COMM_AWAITING_ACK &&
                             device.lastTxTimestamp == ackPkt->originalTxMicros);
            if (accepted) {
                device.successfulAcks++;
                device.commStatus = COMM_ACK_RECEIVED_SUCCESS;
            }
            taskEXIT_CRITICAL(&rdMux);
            if (accepted) {
                logPrintf(LogLevel::LOG_INFO, "COMM RX: ACK OK Dev %d (try %d, %lums, RSSI %ddBm)",
                          device.deviceID, device.sendAttempts,
                          millis() - device.lastPacketSendTime,
                          (info && info->rx_ctrl) ? info->rx_ctrl->rssi : 0);
            }
            return; // 중복 ACK는 조용히 무시
        }
    }
}

bool initEspNow() {
    if (espNowInitialized) return true;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    if (esp_now_init() != ESP_OK) return false;

    // [CRITICAL] WiFi 절전(모뎀 슬립) 비활성화 — WiFi가 완전히 시작된 뒤(esp_now_init 후)에
    // 호출해야 확실히 적용됨. 부팅 직후 너무 일찍 호출하면 적용 실패 → 송신기가 ACK를
    // 간헐적으로 못 받는 문제 발생(보내기는 되는데 받기가 안 됨).
    esp_wifi_set_ps(WIFI_PS_NONE);
    // [발열/전류] 근거리(-50dBm 수신)엔 최대출력 과함 → 13dBm로 낮춤.
    // 최대출력의 전류 스파이크가 순간 브라운아웃·발열을 유발할 수 있어 적정값으로.
    esp_wifi_set_max_tx_power(52); // 52 = 13dBm (충분히 강함)

    esp_now_register_send_cb(espNowSendCb);
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) return false;
    espNowInitialized = true;
    return true;
}

// [NEW] FIRE 패킷을 4회 버스트로 송출하는 공용 헬퍼 (초기 송신 + 재시도에서 공용 사용).
// 매 호출마다 새 txMicros를 부여하고 device.lastTxTimestamp를 갱신하여,
// 재시도 시에도 수신기가 보내는 ACK(originalTxMicros)가 정확히 매칭되도록 한다.
// 수신기는 txButtonPressMicros(버튼 누른 시각)로 중복을 판별하므로 재시도가 중복 발사를 유발하지 않는다.
void sendFireBurst(RunningDevice& device) {
    // [중요] 송신 직전 esp_wifi_set_channel 호출 제거 — 채널은 init/reinit에서 이미 고정돼 있고,
    // 매 송신마다 채널 재설정하면 직후 도착하는 ACK 수신을 놓칠 수 있음(첫 ACK 상시 유실 원인 의심).
    Comm::CommPacket packet;
    const DeviceSettings& settings = deviceSettings[device.deviceID];
    Comm::fillPacket(packet, Comm::FIRE_COMMAND, device.deviceID,
                     device.txButtonPressSequenceMicros, settings);

    device.lastTxTimestamp = packet.txMicros; // ACK 매칭용 최신 txMicros 갱신

    // [신뢰성] FIRE를 4회 ~2ms 간격으로 분산(~6ms에 종료) → 간섭에 강하면서도 송신 시간이 짧아
    // 직후 ACK를 받을 수 있음. (수신기는 송신기 버스트가 끝나도록 ACK를 ~12ms 지연 송출함.)
    // 동일 txMicros라 수신기는 1회만 ACK(폭주 없음), 실행도 1회만(중복 발사 없음).
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (esp_now_send(broadcastAddress, (uint8_t*)&packet, sizeof(packet)) != ESP_OK) {
            logPrintf(LogLevel::LOG_WARN, "FIRE TX attempt %d failed", attempt + 1);
        }
        if (attempt < 3) delay(2); // 2ms 간격 → ~6ms에 분산 종료
    }
    device.lastPacketSendTime = millis();
}

// [NEW] 홀드 패킷 1회 송출 (킵얼라이브). 누르는 동안 메인루프가 주기적으로 호출.
// active=1: 출력 유지, active=0: 즉시 OFF(뗄 때 보조). 그룹은 멤버 ID별로 각각 호출.
void sendHoldCommand(uint8_t targetId, uint8_t machineType, uint8_t pwm, uint8_t active) {
    Comm::HoldPacket packet;
    Comm::fillHoldPacket(packet, targetId, machineType, pwm, active);
    esp_now_send(broadcastAddress, (uint8_t*)&packet, sizeof(packet));
    esp_now_send(broadcastAddress, (uint8_t*)&packet, sizeof(packet));
}

void sendCancelCommand(uint8_t targetId) {
    Comm::CancelPacket packet;
    Comm::fillCancelPacket(packet, targetId);
    for (int i = 0; i < 3; i++) {
        esp_now_send(broadcastAddress, (uint8_t*)&packet, sizeof(packet));
        if (i < 2) delay(2);
    }
    logPrintf(LogLevel::LOG_INFO, "[CANCEL] Cancel sent to ID %d", targetId);
}

bool manageCommunication() {
    for (int i = 0; i < groupDeviceCount; ++i) {
        RunningDevice& device = runningDevices[i];

        // 이미 전송 처리가 끝났거나 ACK 대기 중인 기기는 패스
        if (device.commStatus == COMM_AWAITING_ACK || device.commStatus == COMM_ACK_RECEIVED_SUCCESS || device.commStatus == COMM_FAILED_NO_ACK) {
            continue;
        }

        if (device.commStatus == COMM_PENDING_FIRE) {
            sendFireBurst(device);
            logPrintf(LogLevel::LOG_INFO, "COMM TX: Sending FIRE to Device %d on Channel %d, txMicros: %u",
                      device.deviceID, WIFI_CHANNEL, device.lastTxTimestamp);

            taskENTER_CRITICAL(&rdMux);
            device.sendAttempts = 1;
            device.commStatus = COMM_AWAITING_ACK;
            taskEXIT_CRITICAL(&rdMux);
        }
    }

    // [NEW] 버스트 전송이 완료되면 ACK를 기다리지 않고 루프를 즉시 완료시킵니다.
    // 수신기 응답(ACK)은 OnDataRecv 콜백을 통해 비동기적으로 취합되어 모니터 화면에 반영됩니다.
    return true;
}

void deinitEspNow() {
    if (espNowInitialized) {
        esp_now_deinit();
        espNowInitialized = false;
        esp_wifi_set_channel(0, WIFI_SECOND_CHAN_NONE);
        WiFi.mode(WIFI_MODE_NULL);
    }
}

// RF 스캔의 반복 채널 변경으로 손상된 ESP-NOW 수신 상태를 복원
void reinitEspNow() {
    logPrintf(LogLevel::LOG_INFO, "ESPNOW: Reinitializing after RF scan...");
    // OnDataRecv(Core 0)가 콜백 중일 수 있으므로: 먼저 플래그를 false로 설정해
    // 새 패킷이 들어와도 콜백 초반에 조기 반환하게 만든 뒤 deinit.
    espNowInitialized = false;
    vTaskDelay(pdMS_TO_TICKS(5)); // 진행 중인 콜백이 끝날 시간 확보
    esp_now_deinit();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (initEspNow()) {
        logPrintf(LogLevel::LOG_INFO, "ESPNOW: Reinitialized successfully.");
    } else {
        logPrintf(LogLevel::LOG_ERROR, "ESPNOW: Reinit failed.");
    }
}

// 무선 복제 (MAC) 신호 송출 - 모든 채널 버스트 전송
void sendCloneAnnounce() {
    Comm::ClonePacket pkt;
    memcpy(pkt.signature, Comm::kSig, 4);
    pkt.version = Comm::kVersion;
    pkt.packetType = Comm::CLONE_MAC_ANNOUNCE;
    esp_read_mac(pkt.macAddress, ESP_MAC_WIFI_STA);
    pkt.crc8 = Comm::crc8((const uint8_t*)&pkt, sizeof(Comm::ClonePacket) - 1);

    logPrintf(LogLevel::LOG_INFO, "CLONE TX: Starting clone announce burst on channels {1, 6, 11}");
    for (uint8_t ch : Comm::HOPPING_CHANNELS) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        logPrintf(LogLevel::LOG_INFO, "CLONE TX: Sending on Ch %d (10 times)", ch);
        // [NEW] 송신 횟수를 채널당 10번으로 늘려서 수신기의 긴 채널 대기(1000ms)와 안정적으로 매칭되도록 보강
        for (int i = 0; i < 10; i++) {
            esp_now_send(broadcastAddress, (uint8_t*)&pkt, sizeof(pkt));
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    logPrintf(LogLevel::LOG_INFO, "CLONE: Announce Sent (All Channels Burst)!");
    // [FIX] 채널 홉(1→6→11) 후 수신 경로 손상 복원 — RF 스캔과 동일 이유로 필요.
    // 누락 시 페어링 화면을 나간 뒤 ACK 수신 불능.
    reinitEspNow();
}

// [NEW] RF 자동 환경 스캔 백그라운드 태스크 구현
static void rfScanTask(void* param) {
    rfScanRunning = true;
    rfScanComplete = false;
    rfScanBestChannel = 1;
    memset((void*)rfScanChannelRates, 0, sizeof(rfScanChannelRates));
    rfScanStatus = RF_SCAN_INIT;
    vTaskDelay(pdMS_TO_TICKS(100));

    // 1단계: Ch 1, 6, 11에서 순차적으로 스캔 개시 신호 브로드캐스트 (전체 수신기 연동)
    uint8_t startChannels[] = {1, 6, 11};
    rfScanStatus = RF_SCAN_SYNCING;
    logPrintf(LogLevel::LOG_INFO, "RF SCAN TX: Phase 1: Sending SCAN_START on Ch 1, 6, 11");
    
    Comm::ScanStartPacket startPkt;
    memcpy(startPkt.signature, Comm::kSig, 4);
    startPkt.version = Comm::kVersion;
    startPkt.packetType = Comm::SCAN_START_COMMAND;
    startPkt.crc8 = Comm::crc8((const uint8_t*)&startPkt, sizeof(startPkt) - 1);

    for (uint8_t targetCh : startChannels) {
        esp_wifi_set_channel(targetCh, WIFI_SECOND_CHAN_NONE);
        logPrintf(LogLevel::LOG_INFO, "RF SCAN TX: SCAN_START targetCh Ch %d", targetCh);
        vTaskDelay(pdMS_TO_TICKS(50)); // 채널 변경 후 안정화 대기
        for (int i = 0; i < 10; i++) {
            esp_now_send(broadcastAddress, (const uint8_t*)&startPkt, sizeof(startPkt));
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    // 2단계: [핵심 개선] 모든 채널을 빠르게 순회(채널당 ~12ms)하며 프루브 송출.
    // 기존엔 송신기가 채널마다 길게 머물러 수신기 측정창과 타이밍을 맞춰야 했고,
    // 위상이 어긋나면 측정이 통째로 빗나갔음(0/0/9 같은 쓰레기 결과).
    // 빠른 순회로 바꾸면 수신기가 어느 채널에 머물든 ~36ms 내에 반드시 프루브가 도달 →
    // 타이밍 동기 불필요, 측정이 안정적.
    uint8_t channels[] = {1, 6, 11};
    const unsigned long MEASURE_DURATION_MS = 5500; // 수신기 측정 총합(3×1.8s)을 덮도록
    logPrintf(LogLevel::LOG_INFO, "RF SCAN TX: Phase 2: Rapid-cycling probes on Ch 1/6/11 for %lu ms", MEASURE_DURATION_MS);
    unsigned long measureStart = millis();
    uint8_t probeIdx = 0;
    // 순회 중 Ch1에 들렀을 때 수신기 리포트를 미리 받으면 즉시 종료(rfScanComplete).
    while (millis() - measureStart < MEASURE_DURATION_MS && !rfScanComplete) {
        for (uint8_t targetCh : channels) {
            esp_wifi_set_channel(targetCh, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(4)); // [중요] 채널 전환 후 라디오 안정화 (안 하면 프루브 유실→0/0/0)
            Comm::ScanProbePacket probePkt;
            memcpy(probePkt.signature, Comm::kSig, 4);
            probePkt.version = Comm::kVersion;
            probePkt.packetType = Comm::SCAN_PROBE_PACKET;
            probePkt.channel = targetCh;
            probePkt.probeIndex = probeIdx;
            probePkt.crc8 = Comm::crc8((const uint8_t*)&probePkt, sizeof(probePkt) - 1);
            esp_now_send(broadcastAddress, (const uint8_t*)&probePkt, sizeof(probePkt));
            vTaskDelay(pdMS_TO_TICKS(12)); // 채널당 ~16ms → 한 바퀴 ~48ms
            if (rfScanComplete) break;
        }
        probeIdx++;
    }

    // 3단계: 리포트 접수 단계.
    // [핵심 FIX] Phase 2의 초고속 채널순회(~115회 set_channel)가 수신 경로를 손상시킬 수 있어,
    // 리포트 대기 전에 ESP-NOW를 재초기화하여 깨끗한 수신 상태를 복원한다.
    // (이게 "리포트 다수 송신했는데 송신기가 0개 수신"의 원인일 수 있음)
    rfScanStatus = RF_SCAN_AWAITING;
    reinitEspNow();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); // 리포트 랑데부 채널 Ch1로 복귀
    logPrintf(LogLevel::LOG_INFO, "RF SCAN TX: Phase 3: Awaiting reports on Ch 1 (Timeout: 6s)...");
    vTaskDelay(pdMS_TO_TICKS(100));

    unsigned long startWait = millis();
    while (millis() - startWait < 6000 && !rfScanComplete) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if ((millis() - startWait) % 1000 < 50) {
            logPrintf(LogLevel::LOG_INFO, "RF SCAN TX: Still awaiting reports... (%lu ms elapsed)", millis() - startWait);
        }
    }

    if (!rfScanComplete) {
        // [안전 핸드셰이크] 리포트 못 받음 → 채널 변경 안 함. 수신기도 commit 못 받아 현재 채널 유지 → 불일치 없음.
        rfScanStatus = RF_SCAN_TIMEOUT;
        logPrintf(LogLevel::LOG_WARN, "RF SCAN: No report. Keeping Ch %d (no change).", WIFI_CHANNEL);
    } else {
        // [안전 핸드셰이크] 리포트 수신 → 수신기에 '확정 채널'을 통보 (Ch1에서 TX→RX 안정 방향으로 다회 송출).
        taskENTER_CRITICAL(&scanMux);
        uint8_t bestCh = rfScanBestChannel;
        taskEXIT_CRITICAL(&scanMux);
        Comm::ChannelCommitPacket commitPkt;
        Comm::fillChannelCommitPacket(commitPkt, bestCh);
        logPrintf(LogLevel::LOG_INFO, "RF SCAN: Committing Ch %d to receivers (15x)...", bestCh);
        for (int i = 0; i < 15; i++) {
            esp_now_send(broadcastAddress, (const uint8_t*)&commitPkt, sizeof(commitPkt));
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        // 확정 후 송신기 채널 적용/저장
        EEPROM.write(EEPROM_WIFI_CHANNEL_ADDR, bestCh);
        EEPROM.commit();
        WIFI_CHANNEL = bestCh;
        logPrintf(LogLevel::LOG_INFO, "RF SCAN: Settled on Ch %d (Saved).", WIFI_CHANNEL);
    }

    // 채널 최종 확정 적용
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

    // 반복 채널 변경으로 손상된 ESP-NOW 수신 상태 복원 (ACK 및 스캔 리포트 수신 불능 방지)
    reinitEspNow();

    rfScanRunning = false;
    rfScanTaskHandle = NULL;
    vTaskDelete(NULL);
}

void startRfScanSequence() {
    if (rfScanTaskHandle == NULL) {
        xTaskCreatePinnedToCore(rfScanTask, "rfScanTask", 6144, NULL, 3, &rfScanTaskHandle, 1);
    }
}

void stopRfScanSequence() {
    if (rfScanTaskHandle != NULL) {
        vTaskDelete(rfScanTaskHandle);
        rfScanTaskHandle = NULL;
    }
    rfScanRunning = false;
    rfScanComplete = false;
    rfScanStatus = RF_SCAN_IDLE;
    // 기본 채널 복원 및 ESP-NOW 수신 상태 복원
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    reinitEspNow();
}
