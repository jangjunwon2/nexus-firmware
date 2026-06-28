// i18n_t.cpp — 7개 언어 문자열 테이블 (UTF-8)
//
// TR[StrId][Language] : 행 = 문자열, 열 = 언어. 열 순서는 Language enum과 동일:
//   { EN, KO, ZH, JA, DE, ES, FR }
// 번역이 빈 문자열("")이면 t()가 영어(LANG_EN)로 폴백한다.

#include "i18n_t.h"
#include "config_t.h"   // EEPROM_LANGUAGE_ADDR, EEPROM

Language currentLanguage = LANG_EN;

// 열 순서: EN, KO, ZH, JA, DE, ES, FR  (Language enum 순서와 반드시 일치)
static const char* const TR[STR_COUNT][LANG_COUNT] = {
    // ── 홈 메뉴 ───────────────────────────────────────────────────────────────
    /* STR_MENU_GROUP_EXEC  */ { "GROUP EXEC",  "그룹 재생",   "分组执行",   "グループ再生",  "GRUPPE",        "EJEC GRUPO",   "EXEC GROUPE" },
    /* STR_MENU_SINGLE_EXEC */ { "EXEC & SETUP","재생/설정",   "执行与设置", "再生＆設定",    "AUSF.&EINST.",  "EJEC&CONF",    "EXEC&CONF"   },
    /* STR_MENU_GROUP_SET   */ { "GROUP SET",   "그룹 설정",   "分组设置",   "グループ設定",  "GRUPPE EINST",  "AJUSTE GRUPO", "REGL GROUPE" },
    /* STR_MENU_TIME_SET    */ { "TIME SET",    "시간 설정",   "时间设置",   "時間設定",      "ZEIT EINST",    "AJUSTE TIEMPO","REGL TEMPS"  },
    /* STR_MENU_AUTO_CH     */ { "AUTO CH",     "자동 채널",   "自动信道",   "自動チャネル",  "AUTO KANAL",    "CANAL AUTO",   "CANAL AUTO"  },
    /* STR_MENU_UPDATE      */ { "UPDATE",      "업데이트",    "更新",       "アップデート",  "UPDATE",        "ACTUALIZAR",   "MISE A JOUR" },
    /* STR_MENU_PAIRING     */ { "PAIRING",     "페어링",      "配对",       "ペアリング",    "KOPPLUNG",      "EMPAREJAR",    "APPAIRAGE"   },
    /* STR_MENU_SPARE_COPY  */ { "SPARE COPY",  "예비 복사",   "备用复制",   "予備コピー",    "KOPIE",         "COPIA",        "COPIE"       },
    /* STR_MENU_LANGUAGE    */ { "LANGUAGE",    "언어",        "语言",       "言語",          "SPRACHE",       "IDIOMA",       "LANGUE"      },
    // ── 언어 선택 화면 ────────────────────────────────────────────────────────
    /* STR_LANG_TITLE       */ { "LANGUAGE",    "언어 선택",   "选择语言",   "言語選択",      "SPRACHE",       "IDIOMA",       "LANGUE"      },
    /* STR_LANG_HINT_SELECT */ { "ENTER:select","ENTER:선택",  "ENTER:选择", "ENTER:選択",    "ENTER:Wahl",    "ENTER:elegir", "ENTER:choix" },
    /* STR_LANG_HINT_BACK   */ { "BACK:exit",   "BACK:나가기", "BACK:退出",  "BACK:戻る",     "BACK:zurueck",  "BACK:salir",   "BACK:sortir" },
    // ── 공통 ──────────────────────────────────────────────────────────────────
    /* STR_COMMON_BACK      */ { "BACK",        "뒤로",        "返回",       "戻る",          "ZURUECK",       "ATRAS",        "RETOUR"      },
    /* STR_COMMON_HOME      */ { "HOME",        "홈",          "主页",       "ホーム",        "START",         "INICIO",       "ACCUEIL"     },
    // ── 화면 제목 ─────────────────────────────────────────────────────────────
    /* STR_TITLE_MEMBERS    */ { "MEMBERS",     "멤버 편집",   "成员",       "メンバー",      "MITGLIEDER",    "MIEMBROS",     "MEMBRES"     },
    /* STR_TITLE_RUNNING    */ { "RUNNING",     "재생중",      "运行中",     "再生中",        "LAUFEN",        "ACTIVO",       "EN COURS"    },
    /* STR_TITLE_SENDING    */ { "SENDING",     "전송중",      "发送中",     "送信中",        "SENDEN",        "ENVIANDO",     "ENVOI"       },
    /* STR_TITLE_DELAY_SET  */ { "DELAY SET",   "대기 설정",   "延迟设置",   "待機設定",      "VERZOG SET",    "CONF ESPERA",  "CONF DELAI"  },
    /* STR_TITLE_PLAY_SET   */ { "PLAY SET",    "재생 설정",   "播放设置",   "再生設定",      "ABSPIEL SET",   "CONF REPRO",   "CONF JEU"    },
    /* STR_TITLE_POWER_SET  */ { "POWER SET",   "파워 설정",   "功率设置",   "パワー設定",    "LEIST SET",     "CONF POTENC",  "CONF PUISS"  },
    /* STR_TITLE_GROUP_SET  */ { "GROUP SET",   "그룹 설정",   "分组设置",   "グループ設定",  "GRUPPE EINST",  "CONF GRUPO",   "CONF GROUPE" },
    /* STR_TITLE_GROUP_DETAIL*/{ "GROUP DETAIL","그룹 상세",   "分组详情",   "グループ詳細",  "GRUPPE DETAIL", "DETALLE GR.",  "DET GROUPE"  },
    // ── 필드 라벨 ─────────────────────────────────────────────────────────────
    /* STR_FIELD_PLAY       */ { "PLAY",        "재생",        "播放",       "再生",          "PLAY",          "REPRO.",       "JOUER"       },
    /* STR_FIELD_STYLE      */ { "STYLE",       "방식",        "模式",       "モード",        "MODUS",         "MODO",         "MODE"        },
    /* STR_FIELD_GROUP      */ { "GROUP",       "그룹",        "分组",       "グループ",      "GRUPPE",        "GRUPO",        "GROUPE"      },
    /* STR_FIELD_MEMBERS_CNT*/ { "MEMBERS",     "멤버",        "成员",       "メンバー",      "MITGL.",        "MIEMBR.",      "MEMBRES"     },
    /* STR_FIELD_ID         */ { "ID",          "ID",          "ID",         "ID",            "ID",            "ID",           "ID"          },
    /* STR_FIELD_STEP       */ { "STEP",        "스텝",        "步骤",       "ステップ",      "SCHRITT",       "PASO",         "PAS"         },
    /* STR_FIELD_TYPE       */ { "TYPE",        "타입",        "类型",       "タイプ",        "TYP",           "TIPO",         "TYPE"        },
    /* STR_FIELD_DELAY      */ { "DELAY",       "대기",        "延迟",       "待機",          "WARTEN",        "ESPERA",       "ATTENTE"     },
    /* STR_FIELD_POWER      */ { "POWER",       "파워",        "功率",       "パワー",        "LEIST.",        "POTENC.",      "PUISS."      },
    /* STR_FIELD_MIN        */ { "MIN",         "분",          "分",         "分",            "MIN",           "MIN",          "MIN"         },
    /* STR_FIELD_SEC        */ { "SEC",         "초",          "秒",         "秒",            "SEK",           "SEG",          "SEC"         },
    /* STR_FIELD_SECOND     */ { "SECOND",      "초",          "秒",         "秒",            "SEK.",          "SEGS.",        "SECS."       },
    // ── 값 ────────────────────────────────────────────────────────────────────
    /* STR_VAL_TIMER        */ { "TIMER",       "타이머",      "定时",       "タイマー",      "TIMER",         "TEMPOR.",      "MINUT."      },
    /* STR_VAL_HOLD         */ { "HOLD",        "홀드",        "持续",       "ホールド",      "HALTEN",        "MANTEN.",      "MAINT."      },
    /* STR_VAL_SINGLE       */ { "SINGLE",      "단일",        "单独",       "単独",          "EINZELN",       "SOLO",         "SEUL"        },
    /* STR_VAL_NONE         */ { "none",        "없음",        "无",         "なし",          "keins",         "ninguno",      "aucun"       },
    /* STR_VAL_YES          */ { "YES",         "포함",        "是",         "はい",          "JA",            "SI",           "OUI"         },
    /* STR_VAL_NO           */ { "NO",          "제외",        "否",         "いいえ",        "NEIN",          "NO",           "NON"         },
    // ── 실행/완료 상태 ────────────────────────────────────────────────────────
    /* STR_STATUS_COMPLETE  */ { "COMPLETE",    "완료",        "完成",       "完了",          "FERTIG",        "COMPLET.",     "TERMINE"     },
    /* STR_STATUS_PARTIAL   */ { "PARTIAL",     "부분완료",    "部分完成",   "部分完了",      "TEILW.",        "PARCIAL",      "PARTIEL"     },
    /* STR_STATUS_FAILED    */ { "FAILED",      "실패",        "失败",       "失敗",          "FEHLER",        "FALLIDO",      "ECHEC"       },
    /* STR_STATUS_NO_DEV    */ { "NO DEV",      "장치없음",    "无设备",     "機器なし",      "KEIN GER.",     "SIN DISP.",    "SANS APP."   },
    /* STR_STATUS_OK        */ { "OK",          "성공",        "成功",       "成功",          "OK",            "OK",           "OK"          },
    /* STR_STATUS_FAIL      */ { "FAIL",        "실패",        "失败",       "失敗",          "FEHLER",        "FALLO",        "ECHEC"       },
    /* STR_STATUS_END       */ { "END",         "종료",        "结束",       "終了",          "ENDE",          "FIN",          "FIN"         },
    /* STR_STATUS_CANCELLED */ { "CANCELLED",   "취소됨",      "已取消",     "キャンセル済",  "ABGEBR.",       "CANCELADO",    "ANNULE"      },
    // ── AUTO CH 화면 ──────────────────────────────────────────────────────────
    /* STR_CH_SCANNING      */ { "Scanning...",     "스캔중...",     "扫描中...",     "スキャン中...",  "Scanne...",       "Escan...",       "Scan..."         },
    /* STR_CH_KEEP_RX       */ { "Keep RX powered", "RX 전원 켜두기", "保持RX供电",    "RX電源維持",     "RX an lassen",    "RX encendido",   "RX allume"       },
    /* STR_CH_DONE_BEST     */ { "Done! Best Ch:",  "완료! 최적:",   "完成!最佳:",    "完了!最良ch:",   "Fertig! Kanal:",  "Hecho! Canal:",  "Fini! Canal:"    },
    /* STR_CH_ENTER_RERUN   */ { "ENTER: re-run",   "ENTER:재실행",  "ENTER:重新扫描", "ENTER:再実行",  "ENTER:neu",       "ENTER:rehacer",  "ENTER:relancer"  },
    /* STR_CH_NO_REPLY      */ { "No RX reply",     "RX 응답없음",   "RX无回应",      "RX応答なし",     "Keine Antw.",     "Sin respuesta",  "Sans reponse"    },
    /* STR_CH_ENTER_RETRY   */ { "ENTER: retry",    "ENTER:재시도",  "ENTER:重试",    "ENTER:再試行",   "ENTER:nochmal",   "ENTER:reinten.", "ENTER:relancer"  },
    /* STR_CH_ENTER_OPT     */ { "ENTER: optimize", "ENTER:최적화",  "ENTER:优化",    "ENTER:最適化",   "ENTER:optimier.", "ENTER:optimizar","ENTER:optimis."  },
    /* STR_CH_BACK_EXIT     */ { "BACK: Exit",      "BACK:나가기",   "BACK:退出",     "BACK:戻る",      "BACK:Beenden",    "BACK:salir",     "BACK:quitter"    },
    // ── 페어링 화면 ───────────────────────────────────────────────────────────
    /* STR_PAIR_SIGNAL_ON   */ { "Pairing signal ON","페어링 신호 ON","配对信号开启",  "ペアリング信号ON","Kopplung aktiv",  "Senal empar.ON", "Signal appar.ON" },
    /* STR_PAIR_LED_HINT    */ { "RX LED off=paired","RX LED꺼짐=완료","RX LED灭=配对","RXLED消灯=完了",  "RX LED aus=OK",   "RXLED apaG.=OK", "RX LED eteint=OK"},
    /* STR_PAIR_BACK_DONE   */ { "BACK when done",  "완료시 BACK",   "完成后按BACK",  "完了後BACK",     "BACK wenn fertig","BACK al terminar","BACK quand fini" },
    // ── 복제(SPARE COPY) 화면 ────────────────────────────────────────────────
    /* STR_CLONE_WAITING    */ { "Waiting MAIN...", "MAIN 대기중...", "等待MAIN...",   "MAIN待機中...",  "Warte auf MAIN",  "Esperando MAIN", "Attend MAIN..."  },
    /* STR_CLONE_UP_RESET   */ { "UP: Reset to MAIN","UP:MAIN 초기화","UP:重置为MAIN", "UP:MAINにリセット","UP: zu MAIN",    "UP: a MAIN",     "UP: vers MAIN"   },
    // ── AUTO CH 추가 ──────────────────────────────────────────────────────────
    /* STR_CH_CURRENT       */ { "Cur Ch:",        "현재 채널:",    "当前信道:",     "現在Ch:",        "Kanal:",          "Canal:",           "Canal:"          },
    // ── OTA / 업데이트 ───────────────────────────────────────────────────────
    /* STR_OTA_LATEST       */ { "Latest:",        "최신:",         "最新:",         "最新:",           "Neueste:",        "Reciente:",        "Dernier:"        },
    /* STR_OTA_CURRENT_VER  */ { "Current:",       "현재:",         "当前:",         "現在:",           "Aktuell:",        "Actual:",          "Actuel:"         },
    /* STR_OTA_CHANGELOG    */ { "Changelog:",     "변경사항:",     "更新日志:",     "更新履歴:",       "Aenderung:",      "Cambios:",         "Journal:"        },
    /* STR_OTA_UP_TO_DATE   */ { "Up to date!",    "최신 버전!",    "已是最新!",     "最新版です!",     "Aktuell!",        "Actualizado!",     "A jour!"         },
    /* STR_OTA_CONFIRM      */ { "CONFIRM UPDATE", "업데이트 확인", "确认更新",      "更新確認",        "UPDATE BEST.",    "CONFIRMAR ACT.",   "CONF. MAJ"       },
    /* STR_OTA_DL_START     */ { "ENTER:download", "ENTER:다운로드","ENTER:下载",    "ENTER:DL開始",    "ENTER:Laden",     "ENTER:descargar",  "ENTER:DL"        },
    /* STR_OTA_DL_WARN1     */ { "DO NOT power off","전원끄지마세요","请勿断电",      "電源OFF禁止",     "Nicht ausschalten","No apagar!",      "Ne pas eteindre" },
    /* STR_OTA_DL_WARN2     */ { "while updating.", "업데이트 중",   "更新进行中",    "更新中",          "beim Aktualisieren","durante la act.", "pendant la MAJ"  },
    /* STR_OTA_WIFI_TITLE   */ { "WI-FI SETUP",    "와이파이 설정", "WiFi设置",      "WiFi設定",        "WLAN EINST.",     "CONFIG. WIFI",     "CONFIG. WIFI"    },
    /* STR_OTA_WIFI_AP      */ { "Connect to AP:", "AP에 연결:",    "连接AP:",       "APに接続:",       "Mit AP verbinden:","Conectar AP:",    "Se conn. AP:"    },
    /* STR_OTA_BACK_CANCEL  */ { "BACK:cancel",    "BACK:취소",     "BACK:取消",     "BACK:キャンセル", "BACK:Abbruch",    "BACK:cancelar",    "BACK:annuler"    },
    /* STR_OTA_CONN_TO      */ { "Connecting to:", "연결 중:",      "正在连接:",     "接続中:",         "Verbinde mit:",   "Conectando a:",    "Connexion a:"    },
    /* STR_OTA_CONNECTING   */ { "Connecting...",  "연결 중...",    "连接中...",     "接続中...",       "Verbinde...",     "Conectando...",    "Connexion..."    },
    /* STR_OTA_CHECKING     */ { "Checking...",    "확인 중...",    "检查中...",     "確認中...",       "Pruefe...",       "Comprobando...",   "Verification..." },
    /* STR_OTA_WAIT         */ { "Please wait...", "기다려 주세요...", "请稍候...",    "お待ちください",  "Bitte warten...", "Por favor esp...", "Veuillez att..." },
    /* STR_OTA_UPDATING     */ { "UPDATING",       "업데이트 중",   "更新中",        "更新中",          "AKTUALISIEREN",   "ACTUALIZANDO",     "MISE A JOUR"     },
    /* STR_OTA_DL           */ { "Downloading...", "다운로드 중...", "下载中...",    "ダウンロード中",  "Lade herunter...", "Descargando...",  "Telechargement.." },
    /* STR_OTA_ERROR        */ { "UPDATE ERROR",   "업데이트 오류", "更新错误",      "更新エラー",      "UPDATE FEHLER",   "ERROR ACT.",       "ERREUR MAJ"      },
    /* STR_OTA_ANY_KEY      */ { "Press any key..","아무 키나 누르세요","按任意键...",   "任意のキーを押",  "Taste druecken",  "Pulsar cualq.",    "Appuyer touche"  },
    /* STR_OTA_SUCCESS      */ { "SUCCESS",        "성공",          "成功",          "成功",            "ERFOLG",          "EXITO",            "SUCCES"          },
    /* STR_OTA_ERR_CHECK    */ { "Check failed",   "버전 체크 실패","检查失败",      "確認失敗",        "Pruef.fehlg.",    "Verif.fallida",    "Verif.echouee"   },
    /* STR_OTA_ERR_PARSE    */ { "Parse failed",   "파싱 실패",     "解析失败",      "解析失敗",        "Parse-Fehler",    "Error analisis",   "Erreur analyse"  },
    /* STR_OTA_ERR_NO_URL   */ { "No firmware URL","URL 없음",      "无固件URL",     "URLなし",         "Keine URL",       "Sin URL",          "Sans URL"        },
    /* STR_OTA_ERR_SPACE    */ { "Not enough space","공간 부족",    "空间不足",      "容量不足",        "Kein Speicher",   "Sin espacio",      "Espace insuff."  },
    /* STR_OTA_ERR_WRITE    */ { "Write failed",   "쓰기 실패",     "写入失败",      "書込み失敗",      "Schreib.fehlg.",  "Escrit.fallida",   "Ecrit.echouee"   },
    /* STR_OTA_ERR_UPDATE   */ { "Update failed",  "업데이트 실패", "更新失败",      "更新失敗",        "Update-Fehler",   "Actu.fallida",     "MAJ echouee"     },
    /* STR_OTA_ERR_SIZE     */ { "Invalid size",   "크기 오류",     "大小无效",      "サイズ無効",      "Ungueltige Gr.",  "Taman.invalido",   "Taille invalid." },
    /* STR_OTA_ERR_DL       */ { "Download failed","다운로드 실패", "下载失败",      "DLに失敗",        "DL-Fehler",       "Descarga fallida", "Telecharg.echou."},
    /* STR_RF_SYNCING       */ { "Syncing...",     "동기화중...",   "同步中...",     "同期中...",       "Sync...",         "Sinc...",          "Sync..."         },
    /* STR_RF_INIT          */ { "Init...",        "초기화...",     "初始化...",     "初期化...",       "Init...",         "Inic...",          "Init..."         },
    /* STR_RF_WAITING       */ { "Waiting...",     "대기중...",     "等待中...",     "待機中...",       "Warte...",        "Espera...",        "Attente..."      },
};

const char* t(StrId id) {
    if (id >= STR_COUNT) return "?";
    const char* s = TR[id][currentLanguage];
    if (s == nullptr || s[0] == '\0') s = TR[id][LANG_EN]; // 폴백
    return s;
}

// 언어 선택 메뉴 표기 (현재는 ASCII 안전 — 폰트 없이도 읽힘).
const char* langDisplayName(Language lang) {
    switch (lang) {
        case LANG_EN: return "English";
        case LANG_KO: return "한국어";
        case LANG_ZH: return "中文";
        case LANG_JA: return "日本語";
        case LANG_DE: return "Deutsch";
        case LANG_ES: return "Español";
        case LANG_FR: return "Français";
        default:      return "?";
    }
}

volatile bool pendingEepromCommit = false;

void loadLanguage() {
    uint8_t v = EEPROM.read(EEPROM_LANGUAGE_ADDR);
    currentLanguage = (v < LANG_COUNT) ? (Language)v : LANG_EN; // 미기록(0xFF)·범위밖 → 영어
}

void setLanguage(Language lang) {
    if (lang >= LANG_COUNT) return;
    currentLanguage = lang;
    EEPROM.write(EEPROM_LANGUAGE_ADDR, (uint8_t)lang);
    pendingEepromCommit = true;
}
