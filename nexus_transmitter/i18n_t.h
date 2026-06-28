// i18n_t.h — 다국어(7개 언어) 문자열 테이블 골격
//
// 사용법: 화면에 표시할 문자열은 코드에 직접 쓰지 말고 StrId를 정의한 뒤
//   display.print(t(STR_XXX));  // 현재 언어의 번역을 반환
// 형태로 호출한다. 번역은 i18n_t.cpp의 TR[][] 표에 7개 언어를 채운다.
//
// ⚠️ 한/중/일(CJK)·악센트 문자는 현재 Adafruit 기본 폰트로는 렌더되지 않는다.
//    문자열은 UTF-8로 올바르게 저장되며, 실제 표시는 폰트 단계(U8g2_for_Adafruit_GFX)에서 완성된다.
//    영어는 지금도 정상 표시되므로 골격(테이블/메뉴/저장) 검증이 가능하다.

#ifndef I18N_T_H
#define I18N_T_H

#include <Arduino.h>

// 지원 언어 (7개). 값 = 언어 선택 메뉴의 표시 순서이기도 함.
// LANG_EN을 0(기본·폴백)으로 둔다 — 번역이 비어 있으면 영어로 폴백.
enum Language : uint8_t {
    LANG_EN = 0,  // English
    LANG_KO,      // 한국어
    LANG_ZH,      // 中文 (简体)
    LANG_JA,      // 日本語
    LANG_DE,      // Deutsch
    LANG_ES,      // Espanol
    LANG_FR,      // Francais
    LANG_COUNT
};

// UI 문자열 ID. 새 문자열은 여기에 추가하고 i18n_t.cpp의 TR[][]에 7개 언어를 채운다.
enum StrId : uint16_t {
    // 홈 메뉴
    STR_MENU_GROUP_EXEC = 0,
    STR_MENU_SINGLE_EXEC,
    STR_MENU_GROUP_SET,
    STR_MENU_TIME_SET,
    STR_MENU_AUTO_CH,
    STR_MENU_UPDATE,
    STR_MENU_PAIRING,
    STR_MENU_SPARE_COPY,
    STR_MENU_LANGUAGE,
    // 언어 선택 화면
    STR_LANG_TITLE,
    STR_LANG_HINT_SELECT,
    STR_LANG_HINT_BACK,
    // 공통
    STR_COMMON_BACK,
    STR_COMMON_HOME,
    // 화면 제목 (displayCenteredModeName 인자)
    STR_TITLE_MEMBERS,
    STR_TITLE_RUNNING,
    STR_TITLE_SENDING,
    STR_TITLE_DELAY_SET,
    STR_TITLE_PLAY_SET,
    STR_TITLE_POWER_SET,
    STR_TITLE_GROUP_SET,
    STR_TITLE_GROUP_DETAIL,
    // 필드 라벨
    STR_FIELD_PLAY,
    STR_FIELD_STYLE,
    STR_FIELD_GROUP,
    STR_FIELD_MEMBERS_CNT,
    STR_FIELD_ID,
    STR_FIELD_STEP,
    STR_FIELD_TYPE,
    STR_FIELD_DELAY,
    STR_FIELD_POWER,
    STR_FIELD_MIN,
    STR_FIELD_SEC,
    STR_FIELD_SECOND,
    // 값 (스타일, 타입)
    STR_VAL_TIMER,
    STR_VAL_HOLD,
    STR_VAL_SINGLE,
    STR_VAL_NONE,
    STR_VAL_YES,
    STR_VAL_NO,
    // 실행/완료 상태
    STR_STATUS_COMPLETE,
    STR_STATUS_PARTIAL,
    STR_STATUS_FAILED,
    STR_STATUS_NO_DEV,
    STR_STATUS_OK,
    STR_STATUS_FAIL,
    STR_STATUS_END,
    STR_STATUS_CANCELLED,
    // AUTO CH 화면 본문
    STR_CH_SCANNING,
    STR_CH_KEEP_RX,
    STR_CH_DONE_BEST,
    STR_CH_ENTER_RERUN,
    STR_CH_NO_REPLY,
    STR_CH_ENTER_RETRY,
    STR_CH_ENTER_OPT,
    STR_CH_BACK_EXIT,
    // 페어링 화면 본문
    STR_PAIR_SIGNAL_ON,
    STR_PAIR_LED_HINT,
    STR_PAIR_BACK_DONE,
    // 복제(SPARE COPY) 화면 본문
    STR_CLONE_WAITING,
    STR_CLONE_UP_RESET,
    // AUTO CH 추가
    STR_CH_CURRENT,
    // OTA / 업데이트 화면
    STR_OTA_LATEST,
    STR_OTA_CURRENT_VER,
    STR_OTA_CHANGELOG,
    STR_OTA_UP_TO_DATE,
    STR_OTA_CONFIRM,
    STR_OTA_DL_START,
    STR_OTA_DL_WARN1,
    STR_OTA_DL_WARN2,
    STR_OTA_WIFI_TITLE,
    STR_OTA_WIFI_AP,
    STR_OTA_BACK_CANCEL,
    STR_OTA_CONN_TO,
    STR_OTA_CONNECTING,
    STR_OTA_CHECKING,
    STR_OTA_WAIT,
    STR_OTA_UPDATING,
    STR_OTA_DL,
    STR_OTA_ERROR,
    STR_OTA_ANY_KEY,
    STR_OTA_SUCCESS,
    STR_OTA_ERR_CHECK,
    STR_OTA_ERR_PARSE,
    STR_OTA_ERR_NO_URL,
    STR_OTA_ERR_SPACE,
    STR_OTA_ERR_WRITE,
    STR_OTA_ERR_UPDATE,
    STR_OTA_ERR_SIZE,
    STR_OTA_ERR_DL,

    STR_RF_SYNCING,
    STR_RF_INIT,
    STR_RF_WAITING,

    STR_COUNT
};

// 현재 선택된 언어 (EEPROM에서 로드, 기본 LANG_EN).
extern Language currentLanguage;

// 현재 언어의 문자열(UTF-8) 반환. 번역이 비어 있으면 영어로 폴백. 범위 밖이면 "?".
const char* t(StrId id);

// 언어 선택 메뉴에 표시할 이름. 지금은 ASCII 안전 표기(폰트 없이도 읽힘) —
// 폰트 단계에서 네이티브 표기(한국어/中文/日本語…)로 교체 예정.
const char* langDisplayName(Language lang);

// EEPROM 로드/저장 (EEPROM_LANGUAGE_ADDR). setLanguage는 즉시 commit.
void loadLanguage();
void setLanguage(Language lang);

extern volatile bool pendingEepromCommit;

#endif // I18N_T_H
