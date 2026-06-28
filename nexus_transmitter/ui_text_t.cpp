// ui_text_t.cpp — 다국어 텍스트 렌더 레이어 구현
//
// 폰트 선택 (언어별, 한 번에 한 언어만 활성) — ~12px로 크기 균형:
//   - 라틴(EN/DE/ES/FR): u8g2_font_6x13_te            (13px, 라틴확장=악센트 포함)
//   - 한국어:  커스텀 비트맵(korean_font.h, 굴림 11px)     → gen_korean_font.py
//   - 중국어:  U8g2 u8g2_font_wqy12_t_gb2312           (검증된 12px CJK 비트맵)
//   - 일본어:  커스텀 비트맵(japanese_font.h, MS Gothic 11px) → gen_japanese_font.py

#include <U8g2_for_Adafruit_GFX.h>
#include "ui_text_t.h"
#include "i18n_t.h"
#include "config_t.h"          // SSD1306_WHITE
#include "hardware_display.h"  // extern Adafruit_SSD1306 display
#include "korean_font.h"       // 커스텀 한글 비트맵 폰트 (gen_korean_font.py)
#include "japanese_font.h"     // 커스텀 일본어 비트맵 폰트 (gen_japanese_font.py)
#include "utils_t.h"           // logPrintf

static U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

void uiTextBegin() {
    u8g2Fonts.begin(display);
    u8g2Fonts.setFontMode(1);                 // 투명 배경
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(SSD1306_WHITE);
    // [진단] 업로드 후 시리얼에 cell WxH가 찍혀야 함. 안 바뀌면 빌드가 새 korean_font.h를 안 잡은 것.
    logPrintf(LogLevel::LOG_INFO, "[i18n] Korean font: cell %dx%d, %d glyphs", KFONT_W, KFONT_H, KFONT_COUNT);
}

bool uiIsCJK() {
    return (currentLanguage == LANG_KO || currentLanguage == LANG_ZH || currentLanguage == LANG_JA);
}

int uiLineHeight() {
    // 전 언어 줄높이 12로 통일 → 메뉴가 모두 4줄((64-16)/12=4)로 일관.
    // KO/ZH/JA: 커스텀 비트맵 CELL_H=12, 라틴:6x13te ~12px — 모두 12줄높이에 맞음.
    return 12;
}

void uiSetLangFont() {
    if (currentLanguage == LANG_ZH) {
        u8g2Fonts.setFont(u8g2_font_wqy12_t_gb2312);
        return;
    }
    u8g2Fonts.setFont(u8g2_font_6x13_te);
}

// ── 공통 UTF-8 디코더 ────────────────────────────────────────────────────────
// 다음 코드포인트를 디코드하고 s 포인터를 전진. 잘못된 시퀀스는 0xFFFD 반환.
static uint32_t utf8Next(const char** s) {
    const uint8_t* p = (const uint8_t*)(*s);
    uint32_t cp; int n;
    if (p[0] < 0x80)              { cp = p[0];        n = 1; }
    else if ((p[0] & 0xE0)==0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0)==0xE0) { cp = p[0] & 0x0F; n = 3; }
    else if ((p[0] & 0xF8)==0xF0) { cp = p[0] & 0x07; n = 4; }
    else { (*s)++; return 0xFFFD; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { (*s)++; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n;
    return cp;
}

// ── 한글 커스텀 렌더 (korean_font.h, 굴림 11px) ──────────────────────────────
static int kfontIndex(uint16_t cp) {
    int lo = 0, hi = KFONT_COUNT - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint16_t v = KFONT_CP[m];
        if (v == cp) return m;
        if (v < cp) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

static int koDrawText(int x, int topY, const char* s) {
    int x0 = x;
    while (*s) {
        uint32_t cp = utf8Next(&s);
        if (cp > 0xFFFF) continue;
        int idx = kfontIndex((uint16_t)cp);
        if (idx < 0) { x += 6; continue; }
        display.drawBitmap(x, topY, KFONT_BMP[idx], KFONT_W, KFONT_H, SSD1306_WHITE);
        x += KFONT_ADV[idx];
    }
    return x - x0;
}

static int koTextWidth(const char* s) {
    int w = 0;
    while (*s) {
        uint32_t cp = utf8Next(&s);
        if (cp > 0xFFFF) continue;
        int idx = kfontIndex((uint16_t)cp);
        w += (idx < 0) ? 6 : KFONT_ADV[idx];
    }
    return w;
}

// ── 일본어 커스텀 렌더 (japanese_font.h, MS Gothic 11px) ─────────────────────
static int jfontIndex(uint16_t cp) {
    int lo = 0, hi = JFONT_COUNT - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint16_t v = JFONT_CP[m];
        if (v == cp) return m;
        if (v < cp) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

static int jaDrawText(int x, int topY, const char* s) {
    int x0 = x;
    while (*s) {
        uint32_t cp = utf8Next(&s);
        if (cp > 0xFFFF) continue;
        int idx = jfontIndex((uint16_t)cp);
        if (idx < 0) { x += 6; continue; }
        display.drawBitmap(x, topY, JFONT_BMP[idx], JFONT_W, JFONT_H, SSD1306_WHITE);
        x += JFONT_ADV[idx];
    }
    return x - x0;
}

static int jaTextWidth(const char* s) {
    int w = 0;
    while (*s) {
        uint32_t cp = utf8Next(&s);
        if (cp > 0xFFFF) continue;
        int idx = jfontIndex((uint16_t)cp);
        w += (idx < 0) ? 6 : JFONT_ADV[idx];
    }
    return w;
}
// ─────────────────────────────────────────────────────────────────────────────

void uiPrint(int x, int topY, const char* s) {
    if (currentLanguage == LANG_KO) { koDrawText(x, topY, s); return; }
    if (currentLanguage == LANG_JA) { jaDrawText(x, topY, s); return; }
    uiSetLangFont();
    u8g2Fonts.setForegroundColor(SSD1306_WHITE);
    u8g2Fonts.drawUTF8(x, topY + u8g2Fonts.getFontAscent(), s);
}

int uiTextWidth(const char* s) {
    if (currentLanguage == LANG_KO) return koTextWidth(s);
    if (currentLanguage == LANG_JA) return jaTextWidth(s);
    uiSetLangFont();
    return u8g2Fonts.getUTF8Width(s);
}

void uiPrintForLang(int x, int topY, Language lang, const char* s) {
    if (lang == LANG_KO) { koDrawText(x, topY, s); return; }
    if (lang == LANG_JA) { jaDrawText(x, topY, s); return; }
    Language saved = currentLanguage;
    currentLanguage = lang;
    uiSetLangFont();
    currentLanguage = saved;
    u8g2Fonts.setForegroundColor(SSD1306_WHITE);
    u8g2Fonts.drawUTF8(x, topY + u8g2Fonts.getFontAscent(), s);
}

void uiPrintCentered(int topY, const char* s) {
    int w = uiTextWidth(s);
    int x = (DISPLAY_WIDTH - w) / 2;
    if (x < 0) x = 0;
    uiPrint(x, topY, s);
}
