// transmitter/hardware_display.cpp

#include "config_t.h"
#include "hardware_display.h"
#include "hardware_core.h"
#include "utils_t.h"
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
        groupDisplayTotalLines[i] = (memberCount > 5) ? 2 : 1;
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
            case MODE_TIME_SETTING_DEVICE_TYPE: break;
            case MODE_MULTI_ACTION_OVERVIEW:    break;
            case MODE_MULTI_ACTION_DETAIL:      break;
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
            case MODE_OTA_SUCCESS:          displayCenteredModeName("SUCCESS"); break;
            
            // [NEW] 복제 모드 디스플레이
            case MODE_CLONE_TX:             displayCloneTxMode(); break;
            case MODE_CLONE_RX:             displayCloneRxMode(); break;

            case MODE_EXECUTION:            displayExecutionMode(); break;
            case MODE_TIMER_MONITOR:        displayTimerMonitor(); break;
            case MODE_COMPLETION_MESSAGE:   displayCompletionMessage(); break;
            case MODE_ERROR:
                displayCenteredModeName("ERROR");
                display.setCursor(0, 20);
                display.println("An error occurred!");
                break;
        }
        display.display();
        lastMode = currentMode;
        lastUpdateTime = millis();
    }
}

// [NEW] 무선 복제 UI 추가
void displayCloneTxMode() {
    displayCenteredModeName("SYNC (MAIN)");
    display.setCursor(TEXT_X, OLED_MENU_START_Y + 9);
    display.println("Press ENTER to Sync"); // 동기화하려면 ENTER 누르세요
    display.setCursor(TEXT_X, OLED_MENU_START_Y + 27);
    display.println("Press BACK to Exit");
}

void displayCloneRxMode() {
    displayCenteredModeName("SYNC (SPARE)");
    display.setCursor(TEXT_X, OLED_MENU_START_Y);
    display.println("Waiting for MAIN..."); // 메인 기기 기다리는 중...
    display.setCursor(TEXT_X, OLED_MENU_START_Y + 18);
    display.println("BACK: Exit");
    display.setCursor(TEXT_X, OLED_MENU_START_Y + 27);
    display.println("UP: Reset to MAIN"); // 초기화 시 직관적인 설명
}

// [MODIFIED] 스크롤 처리가 추가된 홈 메뉴 (7개 항목 지원)
void displayHomeMenu() {
    displayCenteredModeName("HOME");
    // 직관적인 이름으로 변경: "SYNC (MAIN)", "SYNC (SPARE)"
    const char* menuItems[] = {"GROUP EXEC", "SINGLE EXEC", "GROUP SET", "TIME SET", "UPDATE", "SYNC (MAIN)", "SYNC (SPARE)"};
    int maxVisible = 5;
    int startIdx = (uiState.menuCursor >= maxVisible) ? (uiState.menuCursor - maxVisible + 1) : 0;
    
    for (int i = 0; i < maxVisible && (startIdx + i) < HOME_MENU_ITEM_COUNT; i++) {
        int itemIdx = startIdx + i;
        display.setCursor(TEXT_X, OLED_MENU_START_Y + (i * OLED_LINE_HEIGHT));
        display.print(menuItems[itemIdx]);
        if (itemIdx == uiState.menuCursor) {
            display.setCursor(CURSOR_X, OLED_MENU_START_Y + (i * OLED_LINE_HEIGHT));
            display.print(">");
        }
    }
}

// ... 아래의 기존 함수들은 전혀 변경되지 않았습니다 (스크롤을 위해 유지) ...
void displayTimerMonitor() {
    displayCenteredModeName("RUNNING");
    const int CONTENT_START_Y = OLED_MENU_START_Y;
    const int MAX_VISIBLE_LINES = (DISPLAY_HEIGHT - CONTENT_START_Y) / OLED_LINE_HEIGHT;
    int activeCount = groupDeviceCount;
    int maxScroll = std::max(0, activeCount - MAX_VISIBLE_LINES);
    uiState.scrollOffset = std::min(uiState.scrollOffset, maxScroll);
    uiState.scrollOffset = std::max(0, uiState.scrollOffset);
    int displayedLine = 0;
    for (int i = 0; i < groupDeviceCount; i++) {
        if (i < uiState.scrollOffset) continue;
        if (displayedLine >= MAX_VISIBLE_LINES) break;
        RunningDevice& rd = runningDevices[i];
        display.setCursor(0, CONTENT_START_Y + (displayedLine * OLED_LINE_HEIGHT));
        display.printf("%02d:", rd.deviceID);
        if (rd.commStatus != COMM_ACK_RECEIVED_SUCCESS) {
             display.print(" ERR");
        } else if (rd.isFinished) {
             display.print(" END");
        } else {
             unsigned long elapsedMs = (micros() - rd.txButtonPressSequenceMicros) / 1000;
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

void displayGroupExecuteMenu() {
    displayCenteredModeName("GROUP EXEC");
    for (int i = 0; i < 5; i++) {
        display.setCursor(TEXT_X, OLED_MENU_START_Y + (i * OLED_LINE_HEIGHT));
        display.printf("GROUP %d", i + 1);
        if ((i + 1) == selectedGroupNum) {
            display.setCursor(CURSOR_X, OLED_MENU_START_Y + (i * OLED_LINE_HEIGHT));
            display.print(">");
        }
    }
}

void displaySingleExecuteMenu() {
    displayCenteredModeName("SINGLE EXEC");
    const int line1_y = OLED_MENU_START_Y;
    const int line2_y = line1_y + OLED_LINE_HEIGHT;
    const int line3_y = line2_y + OLED_LINE_HEIGHT;
    const int line4_y = line3_y + OLED_LINE_HEIGHT;
    const int line5_y = line4_y + OLED_LINE_HEIGHT;
    const auto& settings = deviceSettings[selectedDevice];
    bool isMulti = settings.deviceType == MULTI_ACTION && settings.stepCount > 1;
    const auto& current_step = settings.steps[uiState.selectedStep];
    display.setCursor(TEXT_X, line1_y);
    display.printf("EXECUTE %d%%", current_step.pwmValue);
    if (adjustingValue && uiState.singleExecCursor == 1) drawBlinkingText(TEXT_X, line2_y, "ID   : %02d", selectedDevice);
    else { display.setCursor(TEXT_X, line2_y); display.printf("ID   : %02d", selectedDevice); }
    display.setCursor(TEXT_X, line3_y);
    if (isMulti) {
        if (adjustingValue && uiState.singleExecCursor == 2) drawBlinkingText(TEXT_X, line3_y, "STEP : %d/%d", uiState.selectedStep + 1, settings.stepCount);
        else display.printf("STEP : %d/%d", uiState.selectedStep + 1, settings.stepCount);
    } else display.print("TYPE : SINGLE");
    display.setCursor(TEXT_X, line4_y);
    display.printf("DELAY: %02dm%02ds", current_step.delayMinutes, current_step.delaySeconds);
    display.setCursor(TEXT_X, line5_y);
    display.printf("PLAY : %02ds", current_step.playSeconds);
    if (!adjustingValue) { display.setCursor(CURSOR_X, line1_y + (uiState.singleExecCursor * OLED_LINE_HEIGHT)); display.print(">"); }
}

void displayGroupSettingOverview() {
    displayCenteredModeName("GROUP SET");
    calculateGroupOverviewMetrics();
    const int CONTENT_START_Y = OLED_MENU_START_Y;
    const int MAX_CONTENT_HEIGHT = DISPLAY_HEIGHT - CONTENT_START_Y;
    const int maxVisibleLines = MAX_CONTENT_HEIGHT / OLED_LINE_HEIGHT;
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
            display.setCursor(TEXT_X, CONTENT_START_Y + (relativeStartLine * OLED_LINE_HEIGHT));
            display.printf("GROUP %d: ", i + 1);
            if (i + 1 == selectedGroupNum) {
                display.setCursor(CURSOR_X, CONTENT_START_Y + (relativeStartLine * OLED_LINE_HEIGHT));
                display.print(">");
            }
        }
        std::vector<uint8_t> members;
        for (uint8_t id = 1; id <= MAX_DEVICES; id++) {
            if ((deviceSettings[id].groupMembershipBitmask >> i) & 0x01) members.push_back(id);
        }
        if (members.empty()) {
            if (relativeStartLine >= 0) {
                display.setCursor(initialIndentX, CONTENT_START_Y + (relativeStartLine * OLED_LINE_HEIGHT));
                display.print("none");
            }
            continue;
        }
        String line1 = "", line2 = "";
        for (size_t m = 0; m < members.size(); ++m) {
            String memberStr = String(members[m]);
            if (m < 5) {
                line1 += memberStr;
                if (m < 4 && m < members.size() - 1) line1 += ",";
            } else if (m < 10) {
                line2 += memberStr;
                if (m < 9 && m < members.size() - 1) line2 += ",";
            }
        }
        if (members.size() > 5) line1 += ",";
        if (relativeStartLine >= 0) {
            display.setCursor(initialIndentX, CONTENT_START_Y + (relativeStartLine * OLED_LINE_HEIGHT));
            display.print(line1);
        }
        if (groupDisplayTotalLines[i] > 1 && (relativeStartLine + 1) >= 0 && (relativeStartLine + 1) < maxVisibleLines) {
             display.setCursor(initialIndentX, CONTENT_START_Y + ((relativeStartLine + 1) * OLED_LINE_HEIGHT));
             display.print(line2);
        }
    }
}

void displayGroupSettingDetail() {
    displayCenteredModeName("GROUP DETAIL");
    display.setCursor(TEXT_X, OLED_MENU_START_Y);
    display.printf("ID   : %02d", selectedDevice);
    display.setCursor(TEXT_X, OLED_MENU_START_Y + OLED_LINE_HEIGHT);
    display.printf("DELAY: %02dm %02ds (S1)", deviceSettings[selectedDevice].steps[0].delayMinutes, deviceSettings[selectedDevice].steps[0].delaySeconds);
    display.setCursor(TEXT_X, OLED_MENU_START_Y + (2 * OLED_LINE_HEIGHT));
    display.printf("PLAY : %02ds (S1)", deviceSettings[selectedDevice].steps[0].playSeconds);
    display.setCursor(TEXT_X, OLED_MENU_START_Y + (3 * OLED_LINE_HEIGHT));
    display.print("GROUP: ");
    if ((deviceSettings[selectedDevice].groupMembershipBitmask >> (selectedGroupNum - 1)) & 0x01) display.print("YES"); else display.print("NO");
    display.setCursor(CURSOR_X, OLED_MENU_START_Y + (3 * OLED_LINE_HEIGHT));
    display.print(">");
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
    displayCenteredModeName("SENDING");
    RunningDevice displayDevices[MAX_GROUP_DEVICES];
    uint8_t currentDisplayCount = 0;
    for (uint8_t i = 0; i < groupDeviceCount; i++) {
        if (currentDisplayCount < MAX_GROUP_DEVICES) displayDevices[currentDisplayCount++] = runningDevices[i];
    }
    sortRunningDevicesForDisplay(displayDevices, currentDisplayCount);
    for (int i = 0; i < currentDisplayCount && i < 4; i++) {
        RunningDevice& rd = displayDevices[i];
        display.setCursor(0, OLED_MENU_START_Y + (i * OLED_LINE_HEIGHT));
        const char* statusStr = "SENDING...";
        if (rd.commStatus == COMM_ACK_RECEIVED_SUCCESS) statusStr = "OK";
        else if (rd.commStatus == COMM_FAILED_NO_ACK) statusStr = "FAIL";
        display.printf("ID%02d: %s", rd.deviceID, statusStr);
    }
}

void displayCompletionMessage() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    int successCount = 0;
    for (int i = 0; i < groupDeviceCount; ++i) {
        if (runningDevices[i].commStatus == COMM_ACK_RECEIVED_SUCCESS) successCount++;
    }
    char primaryMsg[20];
    if (groupDeviceCount == 0) snprintf(primaryMsg, sizeof(primaryMsg), "NO DEV");
    else if (successCount == groupDeviceCount) snprintf(primaryMsg, sizeof(primaryMsg), "COMPLETE");
    else if (successCount > 0) snprintf(primaryMsg, sizeof(primaryMsg), "PARTIAL");
    else snprintf(primaryMsg, sizeof(primaryMsg), "FAILED");
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(primaryMsg, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((DISPLAY_WIDTH - w) / 2, (DISPLAY_HEIGHT - h) / 2);
    display.println(primaryMsg);
}

void displayCenteredModeName(const char* modeName) {
    String name(modeName);
    int reservedRightPixels = 25;
    int availableWidthPixels = DISPLAY_WIDTH - reservedRightPixels;
    int maxCharsAvailable = availableWidthPixels / 6;
    if (name.length() > maxCharsAvailable) name = name.substring(0, maxCharsAvailable);
    int padSide = (maxCharsAvailable - name.length()) / 2;
    display.setCursor(0, 0);
    for (int i = 0; i < padSide; i++) display.print("=");
    display.print(name);
    for (int i = 0; i < padSide + ((maxCharsAvailable - name.length()) % 2); i++) {
        if (display.getCursorX() >= (DISPLAY_WIDTH - reservedRightPixels)) break;
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
    int filledSegments = getBatteryPercentage(batteryVoltage);
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
        display.setCursor(x, y);
        display.print(buffer);
    }
}

void displayTimeSettingDelayEdit() {
    displayCenteredModeName("DELAY SETTING");
    display.setCursor(TEXT_X, OLED_MENU_START_Y);
    display.printf("ID: %02d (Step %d)", selectedDevice, uiState.selectedStep + 1);
    const auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
    display.setCursor(TEXT_X, OLED_MENU_START_Y + OLED_LINE_HEIGHT);
    display.print("MIN: ");
    if (adjustingValue && uiState.adjustingUnit == UNIT_MINUTES) drawBlinkingText(display.getCursorX(), display.getCursorY(), "%02d", step.delayMinutes);
    else display.printf("%02d", step.delayMinutes);
    display.setCursor(TEXT_X, OLED_MENU_START_Y + (2 * OLED_LINE_HEIGHT));
    display.print("SEC: ");
    if (adjustingValue && uiState.adjustingUnit == UNIT_SECONDS) drawBlinkingText(display.getCursorX(), display.getCursorY(), "%02d", step.delaySeconds);
    else display.printf("%02d", step.delaySeconds);
    display.setCursor(CURSOR_X, OLED_MENU_START_Y + (uiState.adjustingUnit == UNIT_MINUTES ? 1 : 2) * OLED_LINE_HEIGHT);
    display.print(">");
}

void displayTimeSettingPlayEdit() {
    displayCenteredModeName("PLAY SETTING");
    display.setCursor(TEXT_X, OLED_MENU_START_Y);
    display.printf("ID: %02d (Step %d)", selectedDevice, uiState.selectedStep + 1);
    const auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
    display.setCursor(TEXT_X, OLED_MENU_START_Y + OLED_LINE_HEIGHT);
    display.print("SECOND: ");
    if (adjustingValue) drawBlinkingText(display.getCursorX(), display.getCursorY(), "%02d", step.playSeconds);
    else display.printf("%02d", step.playSeconds);
    display.setCursor(CURSOR_X, OLED_MENU_START_Y + OLED_LINE_HEIGHT);
    display.print(">");
}

void displayTimeSettingPwmEdit() {
    displayCenteredModeName("POWER SETTING");
    display.setCursor(TEXT_X, OLED_MENU_START_Y);
    display.printf("ID: %02d (Step %d)", selectedDevice, uiState.selectedStep + 1);
    const auto& step = deviceSettings[selectedDevice].steps[uiState.selectedStep];
    display.setCursor(TEXT_X, OLED_MENU_START_Y + 2 * OLED_LINE_HEIGHT);
    display.print("POWER: ");
    if (adjustingValue) drawBlinkingText(display.getCursorX(), display.getCursorY(), "%d%%", step.pwmValue);
    else display.printf("%d%%", step.pwmValue);
    display.setCursor(CURSOR_X, OLED_MENU_START_Y + 2 * OLED_LINE_HEIGHT);
    display.print(">");
}

void displayUpdatePage() {
    displayCenteredModeName("UPDATE");
    display.setCursor(TEXT_X, OLED_MENU_START_Y);
    display.print("Latest: ");
    display.println(otaState.latestVersion);
    display.setCursor(TEXT_X, OLED_MENU_START_Y + OLED_LINE_HEIGHT);
    display.print("Current: ");
    display.println(firmwareVersion);
    display.setCursor(TEXT_X, OLED_MENU_START_Y + (OLED_LINE_HEIGHT * 2));
    if (otaState.changeLog.length() > 0) {
        display.println("Changelog:");
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
        if (lines.size() > MAX_CONTENT_LINES) otaState.scrollOffset = std::min(otaState.scrollOffset, (int)lines.size() - MAX_CONTENT_LINES);
        else otaState.scrollOffset = 0;
        for (int i = 0; i < MAX_CONTENT_LINES; ++i) {
            int line_idx = otaState.scrollOffset + i;
            if (line_idx < (int)lines.size()) {
                display.setCursor(TEXT_X, OLED_MENU_START_Y + (OLED_LINE_HEIGHT * 3) + (i * OLED_LINE_HEIGHT));
                display.print(lines[line_idx]);
            }
        }
    } else if (!otaState.updateAvailable && otaState.latestVersion.length() > 0 && otaState.latestVersion != "N/A") {
        display.println("You have the latest");
        display.setCursor(TEXT_X, OLED_MENU_START_Y + (OLED_LINE_HEIGHT * 3));
        display.println("version.");
    }
}

void displayTimeSettingOverview() {
    displayCenteredModeName("TIME SET");
    auto& settings = deviceSettings[selectedDevice];
    const auto& current_step = settings.steps[uiState.selectedStep];
    const int totalFields = 6;
    struct FieldInfo { const char* name; OledMenuState state; } fields[totalFields] = {
        {"ID", FIELD_ID}, {"TYPE", FIELD_TYPE}, {"STEP", FIELD_STEP}, {"DELAY", FIELD_DELAY}, {"PLAY", FIELD_PLAY}, {"PWM", FIELD_PWM}
    };
    const int MAX_VISIBLE_ITEMS = 5;
    int scrollOffset = 0;
    if (uiState.timeSetCursor >= MAX_VISIBLE_ITEMS) scrollOffset = uiState.timeSetCursor - (MAX_VISIBLE_ITEMS - 1);
    int yPos = OLED_MENU_START_Y;
    for (int i = scrollOffset; i < totalFields && i < scrollOffset + MAX_VISIBLE_ITEMS; ++i) {
        display.setCursor(TEXT_X, yPos);
        if (fields[i].state == FIELD_ID) {
            if (adjustingValue && uiState.timeSetCursor == i) drawBlinkingText(TEXT_X, yPos, "ID   : %02d", selectedDevice);
            else display.printf("ID   : %02d", selectedDevice);
        } else if (fields[i].state == FIELD_TYPE) { 
            const char* typeStr = getMachineTypeStr(settings.machineType);
            if (adjustingValue && uiState.timeSetCursor == i) drawBlinkingText(TEXT_X, yPos, "TYPE : %s", typeStr);
            else display.printf("TYPE : %s", typeStr);
        } else if (fields[i].state == FIELD_STEP) {
             if (adjustingValue && uiState.timeSetCursor == i) {
                if (uiState.adjustingStepCount) { display.print("STEP : "); display.print(uiState.selectedStep + 1); display.print("/"); drawBlinkingText(display.getCursorX(), yPos, "%d", settings.stepCount); }
                else { display.print("STEP : "); drawBlinkingText(display.getCursorX(), yPos, "%d", uiState.selectedStep + 1); display.printf("/%d", settings.stepCount); }
            } else display.printf("STEP : %d/%d", uiState.selectedStep + 1, settings.stepCount);
        } else if (fields[i].state == FIELD_DELAY) display.printf("DELAY: %02dm %02ds", current_step.delayMinutes, current_step.delaySeconds);
        else if (fields[i].state == FIELD_PLAY) display.printf("PLAY : %02ds", current_step.playSeconds);
        else if (fields[i].state == FIELD_PWM) display.printf("PWR  : %d%%", current_step.pwmValue);

        if (!adjustingValue && uiState.timeSetCursor == i) { display.setCursor(CURSOR_X, yPos); display.print(">"); }
        yPos += OLED_LINE_HEIGHT;
    }
    if (scrollOffset > 0) display.fillTriangle(120, 16, 124, 20, 116, 20, SSD1306_WHITE);
    if (scrollOffset + MAX_VISIBLE_ITEMS < totalFields) display.fillTriangle(120, 58, 124, 54, 116, 54, SSD1306_WHITE);
}

void displayOtaConfirm() {
    displayCenteredModeName("CONFIRM UPDATE");
    display.setTextSize(1);
    display.setCursor(0, OLED_MENU_START_Y);
    display.println("ENTER starts download");
    display.setCursor(0, OLED_MENU_START_Y + OLED_LINE_HEIGHT);
    display.println("and auto reboot.");
    display.setCursor(0, OLED_MENU_START_Y + (3 * OLED_LINE_HEIGHT));
    display.println("DO NOT turn off power");
    display.setCursor(0, OLED_MENU_START_Y + (4 * OLED_LINE_HEIGHT));
    display.println("during the update.");
    drawBlinkingText(CURSOR_X, OLED_MENU_START_Y, ">");
}
void displayOtaWifiAp() {
    displayCenteredModeName("WI-FI SETUP");
    display.setCursor(TEXT_X, OLED_MENU_START_Y);
    display.println("Connect to AP:");
    display.setCursor(TEXT_X, OLED_MENU_START_Y + OLED_LINE_HEIGHT);
    display.println(OTA_AP_PREFIX);
    display.setCursor(TEXT_X, OLED_MENU_START_Y + (2 * OLED_LINE_HEIGHT));
    display.println("Go to: 192.168.4.1");
    display.setCursor(0, OLED_MENU_START_Y + (4 * OLED_LINE_HEIGHT));
    display.println("Press BACK to cancel");
}
void displayOtaConnecting() {
    displayCenteredModeName("WI-FI");
    if (otaConnectingSsid.length() > 0) {
        display.setCursor(0, 20);
        display.println("Connecting to:");
        String truncatedSsid = otaConnectingSsid;
        if (truncatedSsid.length() > MAX_CHARS_PER_LINE) truncatedSsid = truncatedSsid.substring(0, MAX_CHARS_PER_LINE);
        int16_t x1, y1; uint16_t w, h;
        display.getTextBounds(truncatedSsid, 0, 0, &x1, &y1, &w, &h);
        display.setCursor((DISPLAY_WIDTH - w) / 2, 35);
        display.print(truncatedSsid);
    } else {
        display.setCursor((DISPLAY_WIDTH - 12 * 6) / 2, (DISPLAY_HEIGHT - 8) / 2);
        display.println("Connecting...");
    }
}
void displayOtaChecking() {
    displayCenteredModeName("UPDATE");
    display.setCursor((DISPLAY_WIDTH - 18 * 6) / 2, (DISPLAY_HEIGHT - 8) / 2 - 5);
    display.println("Checking for updates...");
    display.setCursor((DISPLAY_WIDTH - 15 * 6) / 2, (DISPLAY_HEIGHT - 8) / 2 + 5);
    display.println("(Please wait)");
}
void displayOtaDownloading() {
    displayCenteredModeName("UPDATING");
    const char* downloadText = "Downloading...";
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(downloadText, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((DISPLAY_WIDTH - w) / 2, OLED_MENU_START_Y);
    display.println(downloadText);
    char progressStr[10];
    snprintf(progressStr, sizeof(progressStr), "%d%%", otaState.downloadProgress);
    display.getTextBounds(progressStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((DISPLAY_WIDTH - w) / 2, OLED_MENU_START_Y + OLED_LINE_HEIGHT);
    display.println(progressStr);
    display.drawRect(TEXT_X, OLED_MENU_START_Y + (3 * OLED_LINE_HEIGHT), DISPLAY_WIDTH - (TEXT_X * 2), 10, SSD1306_WHITE);
    display.fillRect(TEXT_X + 2, OLED_MENU_START_Y + (3 * OLED_LINE_HEIGHT) + 2, (DISPLAY_WIDTH - (TEXT_X * 2) - 4) * otaState.downloadProgress / 100, 6, SSD1306_WHITE);
}
void displayOtaError() {
    displayCenteredModeName("UPDATE ERROR");
    String msg = otaErrorMessage;
    int line = 0;
    while(msg.length() > 0 && line < 4){
        String lineStr = msg.substring(0, MAX_CHARS_PER_LINE);
        display.setCursor(0, OLED_MENU_START_Y + (line * OLED_LINE_HEIGHT));
        display.println(lineStr);
        msg.remove(0, MAX_CHARS_PER_LINE);
        line++;
    }
    display.setCursor(0, OLED_MENU_START_Y + (4 * OLED_LINE_HEIGHT));
    display.println("Press any key...");
}
void displayOtaScanning() {
    displayCenteredModeName("WI-FI");
    display.setCursor((DISPLAY_WIDTH - 11 * 6) / 2, (DISPLAY_HEIGHT - 8) / 2);
    display.println("Scanning...");
}