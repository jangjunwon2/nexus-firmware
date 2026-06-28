// ui_text_t.h — U8g2 기반 다국어 텍스트 렌더 레이어
//
// 기존 Adafruit_SSD1306 그래픽(삼각형/선/배터리 아이콘 등)은 그대로 두고,
// "텍스트만" U8g2_for_Adafruit_GFX로 그려 한/중/일·악센트(UTF-8)를 표시한다.
// 좌표는 기존 코드처럼 "좌상단(top y)" 기준으로 받고, 내부에서 U8g2 베이스라인으로 변환한다.
//
// ⚠️ 라이브러리 필요: Arduino IDE 라이브러리 매니저에서 "U8g2_for_Adafruit_GFX" 설치.

#ifndef UI_TEXT_T_H
#define UI_TEXT_T_H

#include <Arduino.h>
#include "i18n_t.h"

// display.begin() 이후 1회 호출 (initDisplay에서).
void uiTextBegin();

// 현재 언어(currentLanguage)에 맞는 폰트 선택. uiPrint 계열이 내부에서 자동 호출.
void uiSetLangFont();

// 현재 언어가 CJK(한/중/일)인지 — 줄 높이/표시 개수 등 레이아웃 분기용.
bool uiIsCJK();

// 현재 언어 기준 권장 줄 높이(px). CJK=16, 라틴=9.
int uiLineHeight();

// 좌상단(x, topY) 기준 UTF-8 문자열 출력.
void uiPrint(int x, int topY, const char* s);

// 가로 중앙 정렬 출력(topY 기준).
void uiPrintCentered(int topY, const char* s);

// 현재 폰트로 문자열 픽셀 폭.
int uiTextWidth(const char* s);

// 지정 언어(lang)의 폰트로 문자열 출력 — 언어 선택 메뉴에서 각국 고유 문자 표시용.
void uiPrintForLang(int x, int topY, Language lang, const char* s);

#endif // UI_TEXT_T_H
