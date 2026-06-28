// transmitter/hardware_display.cpp

#include "config_t.h"
#include "hardware_display.h"
#include "hardware_core.h"
#include "hardware_buttons.h"
#include "utils_t.h"
#include "ui_text_t.h"   // U8g2 다국어 텍스트 렌더
#include <algorithm>
#include <vector>
#include "ota_manager.h"

int groupDisplayLineStarts[5];
int groupDisplayTotalLines[5];

void calculateGroupOverviewMetrics() {
    int currentCalculatedLine = 0;
    for (int i = 0; i < 5; ++i) {
        groupDisplayLineStarts[i] = currentCalculatedLine;
        int memberCount = 0;
        for (uint8_t id = 1; id <= MAX_DEVICES; id++) {
            if ((deviceSettings[id].groupMembershipBitmask >> i) & 0x01) {
                memberCount++;
            }
        }
        groupDisplayTotalLines[i] = (memberCount > 15) ? 4 : (memberCount > 10) ? 3 : (memberCount > 5) ? 2 : 1;
        currentCalculatedLine += groupDisplayTotalLines[i];
    }
}

const char* getMachineTypeStr(MachineType type) {
    switch(type) {
        case TYPE_ALL:      return "ALL";
        case TYPE_POT:      return "POT";
        case TYPE_SMOKE:    return "SMOKE";
        case TYPE_FOUNTAIN: return "FOUNTAIN";
        case TYPE_REEL:     return "REEL";
        case TYPE_MAGNET:   return "MAGNET";
        default:            return "UNKNOWN";
    }
}

void updateDisplay() {
    if (!oledInitialized) return;
    
    static Mode lastMode = MODE_BOOT;
    static unsigned long lastUpdateTime = 0;
    
    unsigned long interval = (currentMode == MODE_TIMER_MONITOR) ? 50 : 100;

    if (currentMode != lastMode || (millis() - lastUpdateTime >= interval)) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        displayBatteryIcon();
        switch (currentMode) {
            case MODE_BOOT:                 displayBootScreen(); break;
            case MODE_HOME_MENU:            displayHomeMenu(); break;
            case MODE_GROUP_EXECUTE_MENU:   displayGroupExecuteMenu(); break;
            case MODE_SINGLE_EXECUTE_MENU:  displaySingleExecuteMenu(); break;
            case MODE_GROUP_SETTING_OVERVIEW: displayGroupSettingOverview(); break;
            case MODE_GROUP_SETTING_DETAIL: displayGroupSettingDetail(); break;
            case MODE_TIME_SETTING_OVERVIEW: displayTimeSettingOverview(); break;
            case MODE_TIME_SETTING_PWM_EDIT: displayTimeSettingPwmEdit(); break;
            case MODE_TIME_SETTING_DELAY_EDIT: displayTimeSettingDelayEdit(); break;
            case MODE_TIME_SETTING_PLAY_EDIT:  displayTimeSettingPlayEdit(); break;
            case MODE_OTA_SCANNING:         displayOtaScanning(); break;
            case MODE_UPDATE_PAGE:          displayUpdatePage(); break;
            case MODE_OTA_CONFIRM:          displayOtaConfirm(); break;
            case MODE_OTA_WIFI_AP:          displayOtaWifiAp(); break;
            case MODE_OTA_CONNECTING:       displayOtaConnecting(); break;
            case MODE_OTA_CHECKING:         displayOtaChecking(); break;
            case MODE_OTA_DOWNLOADING:      displayOtaDownloading(); break;
            case MODE_OTA_ERROR:            displayOtaError(); break;
            case MODE_OTA_UPDATING:         displayOtaDownloading(); break; 
            case MODE_OTA_SUCCESS:          displayCenteredModeName(t(STR_OTA_SUCCESS)); break;
            
            // [NEW] 복제 모드 디스플레이
            case MODE_CLONE_TX:             displayCloneTxMode(); break;
            case MODE_CLONE_RX:             displayCloneRxMode(); break;

            // [NEW] RF 자동 환경 분석 스캔 모드 디스플레이
            case MODE_RF_SCAN:              displayRfScanMode(); break;

            // [NEW] 언어 선택 화면
            case MODE_LANGUAGE_SETTING:     displayLanguageMenu(); break;

            // [NEW] 그룹 멤버 편집 화면
            case MODE_GROUP_MEMBERS:        displayGroupMembers(); break;

            case MODE_EXECUTION:            displayExecutionMode(); break;
            case MODE_TIMER_MONITOR:        displayTimerMonitor(); break;
            case MODE_COMPLETION_MESSAGE:   displayCompletionMessage(); break;
            case MODE_CANCEL_MESSAGE:       displayCancelMessage(); break;
            case MODE_ERROR:
                displayCenteredModeName("ERROR");
                uiPrint(0, 20, otaErrorMessage.length() > 0 ? otaErrorMessage.c_str() : "An error occurred!");
                break;
        }
        display.display();
        lastMode = currentMode;
        lastUpdateTime = millis();
    }
}

// [NEW] 무선 복제 UI 추가
void displayCloneTxMode() {
    displayCenteredModeName(t(STR_MENU_PAIRING));
    const int lh = uiLineHeight();
    uiPrint(TEXT_X, OLED_MENU_START_Y,        t(STR_PAIR_SIGNAL_ON));
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh,   t(STR_PAIR_LED_HINT));
    uiPrint(TEXT_X, OLED_MENU_START_Y + 2*lh, t(STR_PAIR_BACK_DONE));
}

void displayCloneRxMode() {
    displayCenteredModeName(t(STR_MENU_SPARE_COPY));
    const int lh = uiLineHeight();
    uiPrint(TEXT_X, OLED_MENU_START_Y,        t(STR_CLONE_WAITING));
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh,   t(STR_CH_BACK_EXIT));
    uiPrint(TEXT_X, OLED_MENU_START_Y + 2*lh, t(STR_CLONE_UP_RESET));
}

// [AUTO CH] 채널 자동 최적화 모드 UI
void displayRfScanMode() {
    displayCenteredModeName(t(STR_MENU_AUTO_CH));
    const int lh = uiLineHeight();
    char buf[24];
    if (rfScanRunning) {
        uiPrint(TEXT_X, OLED_MENU_START_Y,        t(STR_CH_SCANNING));
        {
            const char* scanStatus = (rfScanStatus == RF_SCAN_SYNCING) ? t(STR_RF_SYNCING) :
                                     (rfScanStatus == RF_SCAN_INIT)    ? t(STR_RF_INIT)    : t(STR_RF_WAITING);
            uiPrint(TEXT_X, OLED_MENU_START_Y + lh, scanStatus);
        }
        uiPrint(TEXT_X, OLED_MENU_START_Y + 2*lh, t(STR_CH_KEEP_RX));
    } else if (rfScanComplete) {
        snprintf(buf, sizeof(buf), "%s%d", t(STR_CH_DONE_BEST), rfScanBestChannel);
        uiPrint(TEXT_X, OLED_MENU_START_Y,        buf);
        snprintf(buf, sizeof(buf), "1:%d 6:%d 11:%d", rfScanChannelRates[0], rfScanChannelRates[1], rfScanChannelRates[2]);
        uiPrint(TEXT_X, OLED_MENU_START_Y + lh,   buf);
        uiPrint(TEXT_X, OLED_MENU_START_Y + 2*lh, t(STR_CH_ENTER_RERUN));
        uiPrint(TEXT_X, OLED_MENU_START_Y + 3*lh, t(STR_CH_BACK_EXIT));
    } else if (rfScanStatus == RF_SCAN_TIMEOUT) {
        snprintf(buf, sizeof(buf), "%s%d", t(STR_CH_CURRENT), WIFI_CHANNEL);
        uiPrint(TEXT_X, OLED_MENU_START_Y,        buf);
        uiPrint(TEXT_X, OLED_MENU_START_Y + lh,   t(STR_CH_NO_REPLY));
        uiPrint(TEXT_X, OLED_MENU_START_Y + 2*lh, t(STR_CH_ENTER_RETRY));
        uiPrint(TEXT_X, OLED_MENU_START_Y + 3*lh, t(STR_CH_BACK_EXIT));
    } else {
        snprintf(buf, sizeof(buf), "%s%d", t(STR_CH_CURRENT), WIFI_CHANNEL);
        uiPrint(TEXT_X, OLED_MENU_START_Y,        buf);
        uiPrint(TEXT_X, OLED_MENU_START_Y + lh,   t(STR_CH_ENTER_OPT));
        uiPrint(TEXT_X, OLED_MENU_START_Y + 3*lh, t(STR_CH_BACK_EXIT));
    }
}

// [NEW] 언어 선택 화면 — 홈 메뉴와 동일한 스크롤/커서 방식. 현재 적용 언어엔 " *" 표시.
// 언어 이름은 langDisplayName()의 ASCII 표기라 폰트 없이도 항상 읽힌다.
void displayLanguageMenu() {
    uiPrintCentered(0, t(STR_LANG_TITLE)); // 제목 (CJK 포함 U8g2 렌더)

    const int lineH = uiLineHeight();
    int maxVisible = (DISPLAY_HEIGHT - OLED_MENU_START_Y) / lineH;
    if (maxVisible < 1) maxVisible = 1;
    int startIdx = (uiState.menuCursor >= maxVisible) ? (uiState.menuCursor - maxVisible + 1) : 0;

    for (int i = 0; i < maxVisible && (startIdx + i) < LANG_COUNT; i++) {
        int idx = startIdx + i;
        int y = OLED_MENU_START_Y + (i * lineH);
        char line[28];
        bool isActive = (idx == (int)currentLanguage);
        snprintf(line, sizeof(line), isActive ? "[%s]" : " %s", langDisplayName((Language)idx));
        uiPrintForLang(TEXT_X, y, (Language)idx, line);
        if (idx == uiState.menuCursor) uiPrint(CURSOR_X, y, ">");
    }
    if (startIdx > 0) display.fillTriangle(120, 16, 124, 20, 116, 20, SSD1306_WHITE);
    if (startIdx + maxVisible < LANG_COUNT) display.fillTriangle(120, 58, 124, 54, 116, 54, SSD1306_WHITE);
}

// [MODIFIED] 스크롤 처리가 추가된 홈 메뉴 (9개 항목 지원)
void displayHomeMenu() {
    displayCenteredModeName(t(STR_COMMON_HOME));
    const char* vibItem = vibrationEnabled ? "VIB: ON" : "VIB: OFF";
    const char* menuItems[] = {
        t(STR_MENU_GROUP_EXEC), t(STR_MENU_SINGLE_EXEC),
        t(STR_MENU_AUTO_CH), t(STR_MENU_PAIRING),
        t(STR_MENU_SPARE_COPY), t(STR_MENU_UPDATE), vibItem, t(STR_MENU_LANGUAGE)
    };
    // [다국어] 언어별 글자 크기에 따라 줄높이·표시개수를 동적 계산
    int lineH = uiLineHeight();
    int maxVisible = (DISPLAY_HEIGHT - OLED_MENU_START_Y) / lineH;
    if (maxVisible < 1) maxVisible = 1;
    int startIdx = (uiState.menuCursor >= maxVisible) ? (uiState.menuCursor - maxVisible + 1) : 0;

    for (int i = 0; i < maxVisible && (startIdx + i) < HOME_MENU_ITEM_COUNT; i++) {
        int itemIdx = startIdx + i;
        int y = OLED_MENU_START_Y + (i * lineH);
        uiPrint(TEXT_X, y, menuItems[itemIdx]);
        if (itemIdx == uiState.menuCursor) uiPrint(CURSOR_X, y, ">");
    }
    if (startIdx > 0) display.fillTriangle(120, 16, 124, 20, 116, 20, SSD1306_WHITE);
    if (startIdx + maxVisible < HOME_MENU_ITEM_COUNT) display.fillTriangle(120, 58, 124, 54, 116, 54, SSD1306_WHITE);
}

// ... 아래의 기존 함수들은 전혀 변경되지 않았습니다 (스크롤을 위해 유지) ...
void displayTimerMonitor() {
    displayCenteredModeName(t(STR_TITLE_RUNNING));
    const int CONTENT_START_Y = OLED_MENU_START_Y;
    const int lh = uiLineHeight();
    const int MAX_VISIBLE_LINES = (DISPLAY_HEIGHT - CONTENT_START_Y) / lh;
    int activeCount = groupDeviceCount;
    int maxScroll = std::max(0, activeCount - MAX_VISIBLE_LINES);
    uiState.scrollOffset = std::min(uiState.scrollOffset, maxScroll);
    uiState.scrollOffset = std::max(0, uiState.scrollOffset);
    int displayedLine = 0;
    for (int i = 0; i < groupDeviceCount; i++) {
        if (i < uiState.scrollOffset) continue;
        if (displayedLine >= MAX_VISIBLE_LINES) break;
        RunningDevice& rd = runningDevices[i];
        display.setCursor(0, CONTENT_START_Y + (displayedLine * lh));
        display.printf("%02d:", rd.deviceID);
        if (rd.commStatus == COMM_FAILED_NO_ACK) {
             // 모든 재시도까지 실패한 경우에만 ERR 표시
             display.print(" ERR");
        } else if (rd.commStatus == COMM_PENDING_FIRE || rd.commStatus == COMM_AWAITING_ACK) {
             // 전송/응답 대기 중 (정상 진행) — ERR로 오인되지 않도록 대기 표시
             display.print(" ...");
        } else if (rd.isFinished) {
             display.print(" END");
        } else {
             unsigned long elapsedMs = millis() - rd.startTimeMs;
             const DeviceSettings& settings = deviceSettings[rd.deviceID];
             unsigned long totalStepTime = 0;
             bool inStep = false;
             for(int s=0; s<settings.stepCount; s++) {
                unsigned long rawDelayMs = (long)settings.steps[s].delayMinutes*60000 + settings.steps[s].delaySeconds*1000;
                unsigned long d = rawDelayMs; 
                unsigned long p = (long)settings.steps[s].playSeconds*1000;
                if (elapsedMs < (totalStepTime + d)) {
                    unsigned long remaining = (totalStepTime + d) - elapsedMs;
                    unsigned long displaySec = (remaining + 999) / 1000;
                    display.printf(" D-%02lu", displaySec); 
                    inStep = true;
                    break;
                } else if (elapsedMs < (totalStepTime + d + p)) {
                    unsigned long remaining = (totalStepTime + d + p) - elapsedMs;
                    display.printf(" P-%02lu", remaining/1000); 
                    inStep = true;
                    break;
                }
                totalStepTime += (d + p);
             }
             if(!inStep) display.print(" END");
        }
        displayedLine++;
    }
}

void displayBootScreen() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds("Nexus", 0, 0, &x1, &y1, &w, &h);
    display.setCursor((DISPLAY_WIDTH - w) / 2, (DISPLAY_HEIGHT - h) / 2);
    display.println("Nexus");
}

// [개조] 필드형: 0=PLAY, 1=STYLE, 2=GROUP(엔터로 번호변경), 3=MEMBERS(멤버 편집 진입)
void displayGroupExecuteMenu() {
    displayCenteredModeName(t(STR_MENU_GROUP_EXEC));
    const int lh = uiLineHeight();
    const int y0 = OLED_MENU_START_Y;
    int count = 0;
    for (uint8_t id = 1; id <= MAX_DEVICES; id++)
        if ((deviceSettings[id].groupMembershipBitmask >> (selectedGroupNum - 1)) & 0x01) count++;

    char buf[24];
    uiPrint(TEXT_X, y0, t(STR_FIELD_PLAY));

    snprintf(buf, sizeof(buf), "%s: %s", t(STR_FIELD_STYLE),
             t(execStyle == STYLE_HOLD ? STR_VAL_HOLD : STR_VAL_TIMER));
    uiPrint(TEXT_X, y0 + lh, buf);

    snprintf(buf, sizeof(buf), "%s: %d", t(STR_FIELD_GROUP), selectedGroupNum);
    if (adjustingValue && uiState.menuCursor == 2)
        drawBlinkingText(TEXT_X, y0 + 2*lh, "%s", buf);
    else
        uiPrint(TEXT_X, y0 + 2*lh, buf);

    snprintf(buf, sizeof(buf), "%s (%d)", t(STR_FIELD_MEMBERS_CNT), count);
    uiPrint(TEXT_X, y0 + 3*lh, buf);

    if (!adjustingValue) { uiPrint(CURSOR_X, y0 + (uiState.menuCursor * lh), ">"); }
}

// [NEW] 그룹 멤버 편집 — 장치 ID 목록에서 ENTER로 현재 그룹 포함/제외 토글
void displayGroupMembers() {
    displayCenteredModeName(t(STR_TITLE_MEMBERS));
    const int lh = uiLineHeight();
    const int y0 = OLED_MENU_START_Y;
    int maxVisible = (DISPLAY_HEIGHT - y0) / lh;
    int total = MAX_DEVICES;
    int cursor = uiState.menuCursor;
    int startIdx = (cursor >= maxVisible) ? (cursor - maxVisible + 1) : 0;
    for (int i = 0; i < maxVisible && (startIdx + i) < total; i++) {
        int id = startIdx + i + 1;
        bool inGroup = (deviceSettings[id].groupMembershipBitmask >> (selectedGroupNum - 1)) & 0x01;
        display.setCursor(TEXT_X, y0 + i*lh);
        display.printf("[%c] %02d", inGroup ? 'v' : ' ', id);
        if ((startIdx + i) == cursor) { display.setCursor(CURSOR_X, y0 + i*lh); display.print(">"); }
    }
    if (startIdx > 0) display.fillTriangle(120, 16, 124, 20, 116, 20, SSD1306_WHITE);
    if (startIdx + maxVisible < total) display.fillTriangle(120, 58, 124, 54, 116, 54, SSD1306_WHITE);
}

void displaySingleExecuteMenu() {
    displayCenteredModeName(t(STR_MENU_SINGLE_EXEC));
    const int lh = uiLineHeight();
    const int y0 = OLED_MENU_START_Y;
    const auto& s = deviceSettings[selectedDevice];
    const auto& step = s.steps[uiState.selectedStep];
    const int rowCount = 8;

    // 라벨 IDs (rows 1–7): ID → 방식 → STEP → TYPE → DELAY → PLAY → POWER
    StrId labelIds[7] = {
        STR_FIELD_ID, STR_FIELD_STYLE, STR_FIELD_STEP, STR_FIELD_TYPE,
        STR_FIELD_DELAY, STR_FIELD_PLAY, STR_FIELD_POWER
    };
    int maxLabelW = 0;
    for (int r = 0; r < 7; r++) {
        int w = uiTextWidth(t(labelIds[r]));
        if (w > maxLabelW) maxLabelW = w;
    }
    const int LABEL_COL = maxLabelW + 4;

    char vals[8][22];
    snprintf(vals[0], sizeof(vals[0]), "%s  %d%%", t(STR_FIELD_PLAY), step.pwmValue);
    snprintf(vals[1], sizeof(vals[1]), "%02d", selectedDevice);
    snprintf(vals[2], sizeof(vals[2]), "%s", t(execStyle == STYLE_HOLD ? STR_VAL_HOLD : STR_VAL_TIMER));
    snprintf(vals[3], sizeof(vals[3]), "%d/%d", uiState.selectedStep + 1, s.stepCount);
    snprintf(vals[4], sizeof(vals[4]), "%s", getMachineTypeStr(s.machineType));
    snprintf(vals[5], sizeof(vals[5]), "%02dm%02ds", step.delayMinutes, step.delaySeconds);
    snprintf(vals[6], sizeof(vals[6]), "%02ds", step.playSeconds);
    snprintf(vals[7], sizeof(vals[7]), "%d%%", step.pwmValue);

    int maxVisible = (DISPLAY_HEIGHT - y0) / lh;
    int cursor = uiState.singleExecCursor;
    int startIdx = (cursor >= maxVisible) ? (cursor - maxVisible + 1) : 0;
    for (int i = 0; i < maxVisible && (startIdx + i) < rowCount; i++) {
        int r = startIdx + i;
        int yy = y0 + i * lh;
        bool isEditing = adjustingValue && r == cursor;
        bool blink = (millis() / BLINK_INTERVAL_MS) % 2 == 0;
        if (r == 0) {
            uiPrint(TEXT_X, yy, vals[0]);
        } else if (r == 3 && isEditing) {
            // STEP: 편집 중인 숫자만 블링크 — 스텝선택=현재번호, 스텝수조정=총개수
            bool showCur = uiState.adjustingStepCount || blink;
            bool showTot = !uiState.adjustingStepCount || blink;
            uiPrint(TEXT_X, yy, t(labelIds[r - 1]));
            int xv = TEXT_X + LABEL_COL;
            uiPrint(xv, yy, ": ");
            xv += uiTextWidth(": ");
            char cur[4], tot[4];
            snprintf(cur, sizeof(cur), "%d", uiState.selectedStep + 1);
            snprintf(tot, sizeof(tot), "%d", s.stepCount);
            if (showCur) uiPrint(xv, yy, cur);
            xv += uiTextWidth(cur);
            uiPrint(xv, yy, "/");
            xv += uiTextWidth("/");
            if (showTot) uiPrint(xv, yy, tot);
        } else {
            bool show = !isEditing || blink;
            if (show) {
                char sepval[26];
                snprintf(sepval, sizeof(sepval), ": %s", vals[r]);
                uiPrint(TEXT_X, yy, t(labelIds[r - 1]));
                uiPrint(TEXT_X + LABEL_COL, yy, sepval);
            }
        }
        if (r == cursor && !adjustingValue) { uiPrint(CURSOR_X, yy, ">"); }
    }
    if (startIdx > 0) display.fillTriangle(120, 16, 124, 20, 116, 20, SSD1306_WHITE);
    if (startIdx + maxVisible < rowCount) display.fillTriangle(120, 58, 124, 54, 116, 54, SSD1306_WHITE);
}

void displayGroupSettingOverview() {
    displayCenteredModeName(t(STR_TITLE_GROUP_SET));
    calculateGroupOverviewMetrics();
    const int CONTENT_START_Y = OLED_MENU_START_Y;
    const int lh = uiLineHeight();
    const int MAX_CONTENT_HEIGHT = DISPLAY_HEIGHT - CONTENT_START_Y;
    const int maxVisibleLines = MAX_CONTENT_HEIGHT / lh;
    const int initialIndentX = TEXT_X + (9 * 6);
    int totalContentLines = groupDisplayLineStarts[4] + groupDisplayTotalLines[4];
    int maxScrollOffset = std::max(0, totalContentLines - maxVisibleLines);
    uiState.scrollOffset = std::min(uiState.scrollOffset, maxScrollOffset);
    uiState.scrollOffset = std::max(0, uiState.scrollOffset);
    for (int i = 0; i < 5; ++i) {
        int groupStartLineAbsolute = groupDisplayLineStarts[i];
        int relativeStartLine = groupStartLineAbsolute - uiState.scrollOffset;
        if (relativeStartLine >= maxVisibleLines) continue;
        if (relativeStartLine >= 0) {
            display.setCursor(TEXT_X, CONTENT_START_Y + (relativeStartLine * lh));
            display.printf("GROUP %d: ", i + 1);
            if (i + 1 == selectedGroupNum) {
                display.setCursor(CURSOR_X, CONTENT_START_Y + (relativeStartLine * lh));
                display.print(">");
            }
        }
        std::vector<uint8_t> members;
        for (uint8_t id = 1; id <= MAX_DEVICES; id++) {
            if ((deviceSettings[id].groupMembershipBitmask >> i) & 0x01) members.push_back(id);
        }
        if (members.empty()) {
            if (relativeStartLine >= 0) {
                uiPrint(initialIndentX, CONTENT_START_Y + (relativeStartLine * lh), t(STR_VAL_NONE));
            }
            continue;
        }
        String lines[4] = {"", "", "", ""};
        for (size_t m = 0; m < members.size(); ++m) {
            int slot = m / 5;
            if (slot > 3) break;
            if (m % 5 != 0) lines[slot] += ",";
            lines[slot] += String(members[m]);
        }
        for (int slot = 0; slot < 3; ++slot) {
            if (!lines[slot].isEmpty() && (size_t)((slot + 1) * 5) < members.size()) lines[slot] += ",";
        }
        for (int slot = 0; slot < groupDisplayTotalLines[i]; ++slot) {
            int lineY = relativeStartLine + slot;
            if (lineY >= 0 && lineY < maxVisibleLines) {
                display.setCursor(initialIndentX, CONTENT_START_Y + (lineY * lh));
                display.print(lines[slot]);
            }
        }
    }
}

void displayGroupSettingDetail() {
    displayCenteredModeName(t(STR_TITLE_GROUP_DETAIL));
    const int lh = uiLineHeight();
    const auto& s0 = deviceSettings[selectedDevice].steps[0];
    char buf[28];
    snprintf(buf, sizeof(buf), "%s   : %02d", t(STR_FIELD_ID), selectedDevice);
    uiPrint(TEXT_X, OLED_MENU_START_Y, buf);
    snprintf(buf, sizeof(buf), "%s: %02dm %02ds (S1)", t(STR_FIELD_DELAY), s0.delayMinutes, s0.delaySeconds);
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh, buf);
    snprintf(buf, sizeof(buf), "%s : %02ds (S1)", t(STR_FIELD_PLAY), s0.playSeconds);
    uiPrint(TEXT_X, OLED_MENU_START_Y + 2 * lh, buf);
    bool inGroup = (deviceSettings[selectedDevice].groupMembershipBitmask >> (selectedGroupNum - 1)) & 0x01;
    snprintf(buf, sizeof(buf), "%s: %s", t(STR_FIELD_GROUP), t(inGroup ? STR_VAL_YES : STR_VAL_NO));
    uiPrint(TEXT_X, OLED_MENU_START_Y + 3 * lh, buf);
    uiPrint(CURSOR_X, OLED_MENU_START_Y + 3 * lh, ">");
}

void sortRunningDevicesForDisplay(RunningDevice arr[], uint8_t count) {
    if (count < 2) return;
    std::sort(arr, arr + count, [](const RunningDevice& a, const RunningDevice& b) {
        if (a.commStatus == COMM_FAILED_NO_ACK && b.commStatus != COMM_FAILED_NO_ACK) return true;
        if (a.commStatus != COMM_FAILED_NO_ACK && b.commStatus == COMM_FAILED_NO_ACK) return false;
        if (a.commStatus == COMM_ACK_RECEIVED_SUCCESS && b.commStatus != COMM_ACK_RECEIVED_SUCCESS) return false;
        if (a.commStatus != COMM_ACK_RECEIVED_SUCCESS && b.commStatus == COMM_ACK_RECEIVED_SUCCESS) return true;
        return a.deviceID < b.deviceID;
    });
}

void displayExecutionMode() {
    displayCenteredModeName(t(STR_TITLE_SENDING));
    RunningDevice displayDevices[MAX_GROUP_DEVICES];
    uint8_t currentDisplayCount = 0;
    for (uint8_t i = 0; i < groupDeviceCount; i++) {
        if (currentDisplayCount < MAX_GROUP_DEVICES) displayDevices[currentDisplayCount++] = runningDevices[i];
    }
    sortRunningDevicesForDisplay(displayDevices, currentDisplayCount);
    const int lh = uiLineHeight();
    int maxItems = (DISPLAY_HEIGHT - OLED_MENU_START_Y) / lh;
    for (int i = 0; i < currentDisplayCount && i < maxItems; i++) {
        RunningDevice& rd = displayDevices[i];
        const char* statusStr = t(STR_TITLE_SENDING);
        if (rd.commStatus == COMM_ACK_RECEIVED_SUCCESS) statusStr = t(STR_STATUS_OK);
        else if (rd.commStatus == COMM_FAILED_NO_ACK)   statusStr = t(STR_STATUS_FAIL);
        char buf[20];
        snprintf(buf, sizeof(buf), "ID%02d: %s", rd.deviceID, statusStr);
        uiPrint(0, OLED_MENU_START_Y + (i * lh), buf);
    }
}

void displayCompletionMessage() {
    display.clearDisplay();
    int successCount = 0;
    for (int i = 0; i < groupDeviceCount; ++i) {
        if (runningDevices[i].commStatus == COMM_ACK_RECEIVED_SUCCESS) successCount++;
    }
    StrId msgId;
    if (groupDeviceCount == 0)                    msgId = STR_STATUS_NO_DEV;
    else if (successCount == groupDeviceCount)     msgId = STR_STATUS_COMPLETE;
    else if (successCount > 0)                    msgId = STR_STATUS_PARTIAL;
    else                                           msgId = STR_STATUS_FAILED;
    uiPrintCentered((DISPLAY_HEIGHT - uiLineHeight()) / 2, t(msgId));
}

void displayCancelMessage() {
    uiPrintCentered((DISPLAY_HEIGHT - uiLineHeight()) / 2, t(STR_STATUS_CANCELLED));
}

void displayCenteredModeName(const char* modeName) {
    const int reservedRight = 25;
    const int availW = DISPLAY_WIDTH - reservedRight; // 103px
    const int eqW = 6;
    int titleW = uiTextWidth(modeName);
    if (titleW > availW) titleW = availW;
    int leftPads  = (availW - titleW) / 2 / eqW;
    int rightPads = (availW - leftPads * eqW - titleW) / eqW;
    const int eqY = 2; // 8px Adafruit "=" 를 12px 타이틀 행 중앙에 위치
    display.setCursor(0, eqY);
    for (int i = 0; i < leftPads; i++) display.print("=");
    uiPrint(leftPads * eqW, 0, modeName);
    int afterX = leftPads * eqW + titleW;
    display.setCursor(afterX, eqY);
    for (int i = 0; i < rightPads; i++) {
        if (display.getCursorX() >= availW) break;
        display.print("=");
    }
}

void displayBatteryIcon() {
    const int iconWidth = 15;
    const int iconHeight = 8;
    const int iconGap = 2;
    const int terminalWidth = 2;
    const int x = DISPLAY_WIDTH - iconWidth - terminalWidth - iconGap;
    const int y = 0;
    display.drawRect(x, y + 1, iconWidth, iconHeight - 2, SSD1306_WHITE);
    display.drawRect(x + iconWidth, y + 2, terminalWidth, iconHeight - 4, SSD1306_WHITE);
    int filledSegments = min(batteryPercentage, 4); // 4바 아이콘 범위 초과 방지
    const int segmentWidth = (iconWidth - (5 * 2) - 1) / 4;
    for (int i = 0; i < filledSegments; i++) {
        int segmentX = x + 2 + (i * (segmentWidth + 2));
        display.fillRect(segmentX, y + 3, segmentWidth, iconHeight - 6, SSD1306_WHITE);
    }
}

void drawBlinkingText(int x, int y, const char* format, ...) {
    if ((millis() / BLINK_INTERVAL_MS) % 2 == 0) {
        char buffer[32];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        uiPrint(x, y, buffer);
    }
}

void displayTimeSettingDelayEdit() {
    displayCenteredModeName(t(STR_TITLE_DELAY_SET));
    const int lh = uiLineHeight();
    {
        char devInfo[28];
        snprintf(devInfo, sizeof(devInfo), "%s:%02d  %s:%d", t(STR_FIELD_ID), selectedDevice, t(STR_FIELD_STEP), uiState.selectedStep + 1);
        uiPrint(TEXT_X, OLED_MENU_START_Y, devInfo);
    }
    const auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
    char label[16];
    char numBuf[8];
    // MIN 행
    snprintf(label, sizeof(label), "%s: ", t(STR_FIELD_MIN));
    int lw = uiTextWidth(label);
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh, label);
    snprintf(numBuf, sizeof(numBuf), "%02d", step.delayMinutes);
    if (adjustingValue && uiState.adjustingUnit == UNIT_MINUTES)
        drawBlinkingText(TEXT_X + lw, OLED_MENU_START_Y + lh, "%s", numBuf);
    else uiPrint(TEXT_X + lw, OLED_MENU_START_Y + lh, numBuf);
    // SEC 행
    snprintf(label, sizeof(label), "%s: ", t(STR_FIELD_SEC));
    lw = uiTextWidth(label);
    uiPrint(TEXT_X, OLED_MENU_START_Y + 2*lh, label);
    snprintf(numBuf, sizeof(numBuf), "%02d", step.delaySeconds);
    if (adjustingValue && uiState.adjustingUnit == UNIT_SECONDS)
        drawBlinkingText(TEXT_X + lw, OLED_MENU_START_Y + 2*lh, "%s", numBuf);
    else uiPrint(TEXT_X + lw, OLED_MENU_START_Y + 2*lh, numBuf);
    uiPrint(CURSOR_X, OLED_MENU_START_Y + (uiState.adjustingUnit == UNIT_MINUTES ? 1 : 2) * lh, ">");
}

void displayTimeSettingPlayEdit() {
    displayCenteredModeName(t(STR_TITLE_PLAY_SET));
    const int lh = uiLineHeight();
    {
        char devInfo[28];
        snprintf(devInfo, sizeof(devInfo), "%s:%02d  %s:%d", t(STR_FIELD_ID), selectedDevice, t(STR_FIELD_STEP), uiState.selectedStep + 1);
        uiPrint(TEXT_X, OLED_MENU_START_Y, devInfo);
    }
    const auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
    char label[16];
    snprintf(label, sizeof(label), "%s: ", t(STR_FIELD_SECOND));
    int lw = uiTextWidth(label);
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh, label);
    char numBuf[8];
    snprintf(numBuf, sizeof(numBuf), "%02d", step.playSeconds);
    if (adjustingValue) drawBlinkingText(TEXT_X + lw, OLED_MENU_START_Y + lh, "%s", numBuf);
    else uiPrint(TEXT_X + lw, OLED_MENU_START_Y + lh, numBuf);
    uiPrint(CURSOR_X, OLED_MENU_START_Y + lh, ">");
}

void displayTimeSettingPwmEdit() {
    displayCenteredModeName(t(STR_TITLE_POWER_SET));
    const int lh = uiLineHeight();
    {
        char devInfo[28];
        snprintf(devInfo, sizeof(devInfo), "%s:%02d  %s:%d", t(STR_FIELD_ID), selectedDevice, t(STR_FIELD_STEP), uiState.selectedStep + 1);
        uiPrint(TEXT_X, OLED_MENU_START_Y, devInfo);
    }
    const auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
    char label[16];
    snprintf(label, sizeof(label), "%s: ", t(STR_FIELD_POWER));
    int lw = uiTextWidth(label);
    uiPrint(TEXT_X, OLED_MENU_START_Y + 2*lh, label);
    char numBuf[8];
    snprintf(numBuf, sizeof(numBuf), "%d%%", step.pwmValue);
    if (adjustingValue) drawBlinkingText(TEXT_X + lw, OLED_MENU_START_Y + 2*lh, "%s", numBuf);
    else uiPrint(TEXT_X + lw, OLED_MENU_START_Y + 2*lh, numBuf);
    uiPrint(CURSOR_X, OLED_MENU_START_Y + 2*lh, ">");
}

void displayUpdatePage() {
    displayCenteredModeName(t(STR_MENU_UPDATE));
    const int lh = uiLineHeight();
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %s", t(STR_OTA_LATEST), otaState.latestVersion.c_str());
    uiPrint(TEXT_X, OLED_MENU_START_Y, buf);
    snprintf(buf, sizeof(buf), "%s %s", t(STR_OTA_CURRENT_VER), firmwareVersion.c_str());
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh, buf);
    if (otaState.changeLog.length() > 0) {
        uiPrint(TEXT_X, OLED_MENU_START_Y + lh * 2, t(STR_OTA_CHANGELOG));
        std::vector<String> lines;
        String currentLine = "";
        for (unsigned int i = 0; i < otaState.changeLog.length(); i++) {
            char c = otaState.changeLog.charAt(i);
            if (c == '\n') { lines.push_back(currentLine); currentLine = ""; }
            else currentLine += c;
        }
        if (currentLine.length() > 0) lines.push_back(currentLine);
        otaState.scrollOffset = std::max(0, otaState.scrollOffset);
        const int MAX_CONTENT_LINES = 2;
        if ((int)lines.size() > MAX_CONTENT_LINES) otaState.scrollOffset = std::min(otaState.scrollOffset, (int)lines.size() - MAX_CONTENT_LINES);
        else otaState.scrollOffset = 0;
        for (int i = 0; i < MAX_CONTENT_LINES; ++i) {
            int line_idx = otaState.scrollOffset + i;
            if (line_idx < (int)lines.size()) {
                uiPrint(TEXT_X, OLED_MENU_START_Y + lh * 3 + i * lh, lines[line_idx].c_str());
            }
        }
    } else if (!otaState.updateAvailable && otaState.latestVersion.length() > 0 && otaState.latestVersion != "N/A") {
        uiPrint(TEXT_X, OLED_MENU_START_Y + lh * 2, t(STR_OTA_UP_TO_DATE));
    }
}

void displayTimeSettingOverview() {
    displayCenteredModeName(t(STR_MENU_TIME_SET));
    auto& settings = deviceSettings[selectedDevice];
    const auto& current_step = settings.steps[uiState.selectedStep];
    const int totalFields = 6;
    const int lh = uiLineHeight();
    struct FieldInfo { OledMenuState state; } fields[totalFields] = {
        {FIELD_ID}, {FIELD_TYPE}, {FIELD_STEP}, {FIELD_DELAY}, {FIELD_PLAY}, {FIELD_PWM}
    };
    const int MAX_VISIBLE_ITEMS = (DISPLAY_HEIGHT - OLED_MENU_START_Y) / lh;
    int scrollOffset = 0;
    if (uiState.timeSetCursor >= MAX_VISIBLE_ITEMS) scrollOffset = uiState.timeSetCursor - (MAX_VISIBLE_ITEMS - 1);
    int yPos = OLED_MENU_START_Y;
    char buf[26];
    for (int i = scrollOffset; i < totalFields && i < scrollOffset + MAX_VISIBLE_ITEMS; ++i) {
        if (fields[i].state == FIELD_ID) {
            if (adjustingValue && uiState.timeSetCursor == i) drawBlinkingText(TEXT_X, yPos, "%s   : %02d", t(STR_FIELD_ID), selectedDevice);
            else { snprintf(buf, sizeof(buf), "%s   : %02d", t(STR_FIELD_ID), selectedDevice); uiPrint(TEXT_X, yPos, buf); }
        } else if (fields[i].state == FIELD_TYPE) {
            const char* typeStr = getMachineTypeStr(settings.machineType);
            if (adjustingValue && uiState.timeSetCursor == i) drawBlinkingText(TEXT_X, yPos, "%s : %s", t(STR_FIELD_TYPE), typeStr);
            else { snprintf(buf, sizeof(buf), "%s : %s", t(STR_FIELD_TYPE), typeStr); uiPrint(TEXT_X, yPos, buf); }
        } else if (fields[i].state == FIELD_STEP) {
            if (adjustingValue && uiState.timeSetCursor == i) {
                if (uiState.adjustingStepCount) {
                    // adjustingStepCount=true: total count blinks, step index is static
                    char prefix[20];
                    snprintf(prefix, sizeof(prefix), "%s : %d/", t(STR_FIELD_STEP), uiState.selectedStep + 1);
                    uiPrint(TEXT_X, yPos, prefix);
                    drawBlinkingText(TEXT_X + uiTextWidth(prefix), yPos, "%d", settings.stepCount);
                } else {
                    // adjustingStepCount=false: step index blinks, total count is static
                    char prefix[16], numStr[4], suffix[8];
                    snprintf(prefix, sizeof(prefix), "%s : ", t(STR_FIELD_STEP));
                    snprintf(numStr, sizeof(numStr), "%d", uiState.selectedStep + 1);
                    snprintf(suffix, sizeof(suffix), "/%d", settings.stepCount);
                    int pw = uiTextWidth(prefix);
                    int nw = uiTextWidth(numStr);
                    uiPrint(TEXT_X, yPos, prefix);
                    drawBlinkingText(TEXT_X + pw, yPos, "%s", numStr);
                    uiPrint(TEXT_X + pw + nw, yPos, suffix);
                }
            } else { snprintf(buf, sizeof(buf), "%s : %d/%d", t(STR_FIELD_STEP), uiState.selectedStep + 1, settings.stepCount); uiPrint(TEXT_X, yPos, buf); }
        } else if (fields[i].state == FIELD_DELAY) {
            snprintf(buf, sizeof(buf), "%s: %02dm %02ds", t(STR_FIELD_DELAY), current_step.delayMinutes, current_step.delaySeconds);
            uiPrint(TEXT_X, yPos, buf);
        } else if (fields[i].state == FIELD_PLAY) {
            snprintf(buf, sizeof(buf), "%s : %02ds", t(STR_FIELD_PLAY), current_step.playSeconds);
            uiPrint(TEXT_X, yPos, buf);
        } else if (fields[i].state == FIELD_PWM) {
            snprintf(buf, sizeof(buf), "%s  : %d%%", t(STR_FIELD_POWER), current_step.pwmValue);
            uiPrint(TEXT_X, yPos, buf);
        }
        if (!adjustingValue && uiState.timeSetCursor == i) { uiPrint(CURSOR_X, yPos, ">"); }
        yPos += lh;
    }
    if (scrollOffset > 0) display.fillTriangle(120, 16, 124, 20, 116, 20, SSD1306_WHITE);
    if (scrollOffset + MAX_VISIBLE_ITEMS < totalFields) display.fillTriangle(120, 58, 124, 54, 116, 54, SSD1306_WHITE);
}

void displayOtaConfirm() {
    displayCenteredModeName(t(STR_OTA_CONFIRM));
    const int lh = uiLineHeight();
    uiPrint(TEXT_X, OLED_MENU_START_Y,          t(STR_OTA_DL_START));
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh * 2, t(STR_OTA_DL_WARN1));
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh * 3, t(STR_OTA_DL_WARN2));
    drawBlinkingText(CURSOR_X, OLED_MENU_START_Y, ">");
}
void displayOtaWifiAp() {
    displayCenteredModeName(t(STR_OTA_WIFI_TITLE));
    const int lh = uiLineHeight();
    uiPrint(TEXT_X, OLED_MENU_START_Y,           t(STR_OTA_WIFI_AP));
    display.setCursor(TEXT_X, OLED_MENU_START_Y + lh + 2);
    display.println(OTA_AP_PREFIX);
    display.setCursor(TEXT_X, OLED_MENU_START_Y + lh * 2 + 2);
    display.println("192.168.4.1");
    uiPrint(TEXT_X, OLED_MENU_START_Y + lh * 3,  t(STR_OTA_BACK_CANCEL));
}
void displayOtaConnecting() {
    displayCenteredModeName("WI-FI");
    if (otaConnectingSsid.length() > 0) {
        uiPrint(TEXT_X, OLED_MENU_START_Y, t(STR_OTA_CONN_TO));
        // SSID는 DISPLAY_WIDTH 범위 안에서 uiPrint로 렌더 (CJK SSID도 안전)
        int ssidW = uiTextWidth(otaConnectingSsid.c_str());
        int ssidX = (DISPLAY_WIDTH - ssidW) / 2;
        if (ssidX < 0) ssidX = 0;
        uiPrint(ssidX, OLED_MENU_START_Y + uiLineHeight(), otaConnectingSsid.c_str());
    } else {
        uiPrint((DISPLAY_WIDTH - uiTextWidth(t(STR_OTA_CONNECTING))) / 2, (DISPLAY_HEIGHT - 8) / 2, t(STR_OTA_CONNECTING));
    }
}
void displayOtaChecking() {
    displayCenteredModeName(t(STR_MENU_UPDATE));
    const int midY = (DISPLAY_HEIGHT - 8) / 2;
    uiPrint((DISPLAY_WIDTH - uiTextWidth(t(STR_OTA_CHECKING))) / 2, midY - 5, t(STR_OTA_CHECKING));
    uiPrint((DISPLAY_WIDTH - uiTextWidth(t(STR_OTA_WAIT)))     / 2, midY + 5, t(STR_OTA_WAIT));
}
void displayOtaDownloading() {
    displayCenteredModeName(t(STR_OTA_UPDATING));
    const int lh = uiLineHeight();
    const char* dlText = t(STR_OTA_DL);
    uiPrint((DISPLAY_WIDTH - uiTextWidth(dlText)) / 2, OLED_MENU_START_Y, dlText);
    char progressStr[10];
    snprintf(progressStr, sizeof(progressStr), "%d%%", otaState.downloadProgress);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(progressStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((DISPLAY_WIDTH - w) / 2, OLED_MENU_START_Y + lh);
    display.println(progressStr);
    display.drawRect(TEXT_X, OLED_MENU_START_Y + lh * 3, DISPLAY_WIDTH - (TEXT_X * 2), 10, SSD1306_WHITE);
    display.fillRect(TEXT_X + 2, OLED_MENU_START_Y + lh * 3 + 2, (DISPLAY_WIDTH - (TEXT_X * 2) - 4) * otaState.downloadProgress / 100, 6, SSD1306_WHITE);
}
void displayOtaError() {
    displayCenteredModeName(t(STR_OTA_ERROR));
    const int lh = uiLineHeight();
    // '\n' 기준으로만 분리 — 바이트 절단은 CJK 문자 손상 유발
    const char* p = otaErrorMessage.c_str();
    int line = 0;
    while (*p && line < 4) {
        const char* nl = strchr(p, '\n');
        String chunk = nl ? String(p, (int)(nl - p)) : String(p);
        uiPrint(0, OLED_MENU_START_Y + line * lh, chunk.c_str());
        p = nl ? nl + 1 : p + strlen(p);
        line++;
    }
    uiPrint(0, OLED_MENU_START_Y + lh * 4, t(STR_OTA_ANY_KEY));
}
void displayOtaScanning() {
    displayCenteredModeName("WI-FI");
    const char* scanText = t(STR_CH_SCANNING);
    uiPrint((DISPLAY_WIDTH - uiTextWidth(scanText)) / 2, (DISPLAY_HEIGHT - 8) / 2, scanText);
}