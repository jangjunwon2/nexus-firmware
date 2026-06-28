// transmitter_s3/ota_manager.cpp



#include "config_t.h"

#include <Arduino.h>

#include <WiFi.h>

#include <esp_wifi.h>

#include <esp_event.h>




#include "ota_manager.h"

#include "utils_t.h"

#include <ESPAsyncWebServer.h>

#include <WiFiClientSecure.h>

#include <HTTPClient.h>

#include <ArduinoJson.h>

#include "hardware_display.h"

#include "hardware_buttons.h"

#include "espnow_t.h"

#include "i18n_t.h"

#include <algorithm>

#include <Update.h>

#include <vector>

#include <memory>



// [Fix] ScanResult struct definition needed for vector

struct ScanResult {

    String ssid;

    int32_t rssi;

    bool encrypted;

};



// Mutex for cachedScanResults: written by wifiScanTask (FreeRTOS task, possibly Core 0 or 1),

// read by /api/scan handler (AsyncWebServer, Core 0). Without a mutex this is a race condition.

static SemaphoreHandle_t scanResultsMutex = nullptr;



// Version-check HTTP task runs on Core 0 (WiFi core) so loopTask (Core 1) can keep resetting the


static SemaphoreHandle_t versionCheckSem  = nullptr;

static volatile int      versionCheckCode = 0;

static volatile bool     versionCheckParsed = false;



// [Fix] Forward declarations (함수 원형 선언 추가)

void checkFirmwareVersion();

void performOtaUpdate();

void broadcastWifiStatus(const char* status, int reason = 0);
void broadcastOtaStatus();
void broadcastOtaProgress(int progress);
void broadcastOtaResult(bool success, const String& msg);
static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);


AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

static bool webServerInitialized = false;

static bool wifiEventHandlersRegistered = false;

static wifi_event_id_t wifi_event_handle;



static std::vector<ScanResult> cachedScanResults;

static volatile bool scan_in_progress = false;

static volatile bool checkOtaFromApiFlag = false;

static volatile uint8_t otaExitModeRequest = 0; // 1=MODE_ERROR, 2=MODE_HOME_MENU

static volatile bool otaStartDownloadFlag = false;

static volatile bool otaWifiDisconnectFlag = false;

static volatile bool otaRestartRequest = false;  // /exit 응답 완료 후 메인루프에서 안전하게 재시작

static volatile bool pendingOtaExit = false;      // /exit 핸들러→handleOtaLoop: shutdownWifi+initEspNow 지연 실행

static volatile uint8_t pendingOtaMode = 0;       // WiFi 이벤트(Core 0)→handleOtaLoop(Core 1) 모드 전환 요청

static volatile bool pendingNetworkSave = false;  // WiFi GOT_IP 이벤트에서 자격증명 저장 지연

static volatile bool otaVersionChecked = false;   // GOT_IP 재연결 시 버전 체크 중복 실행 방지

static char pendingNetworkSSID[65] = {0};

static char pendingNetworkPWD[65] = {0};



enum OtaWifiState { OTA_WIFI_IDLE, OTA_WIFI_CONNECTING, OTA_WIFI_CONNECTED, OTA_WIFI_FAILED, OTA_WIFI_DISCONNECTED };

static OtaWifiState otaWifiStatus = OTA_WIFI_IDLE;

static bool otaUpdateDownloaded = false;

static unsigned long wifiConnectStartMillis = 0;




static const char I18N_JS[] PROGMEM = R"rawliteral(
const I18N={

back_home:["Back to Home","홈으로 돌아가기","返回主页","ホームに戻る","Zurück","Volver","Retour"],

pg_home:["Nexus Transmitter","Nexus 송신기","Nexus 发射器","Nexus 送信機","Nexus Sender","Nexus Transmisor","Nexus Émetteur"],

pg_wifi:["Wi-Fi Settings","Wi-Fi 설정","Wi-Fi 设置","Wi-Fi 設定","WLAN Einstellungen","Ajustes Wi-Fi","Paramètres Wi-Fi"],

pg_update:["Firmware Update","펌웨어 업데이트","固件更新","ファームウェア更新","Firmware-Update","Actualización Firmware","Mise à jour Firmware"],

pg_manual:["User Manual","사용 설명서","使用手册","取扱説明書","Benutzerhandbuch","Manual de Usuario","Manuel Utilisateur"],

pg_exit:["Exiting Wi-Fi Mode","Wi-Fi 모드 종료","退出 Wi-Fi 模式","Wi-Fiモード終了","WLAN-Modus beenden","Salir Modo Wi-Fi","Sortie Mode Wi-Fi"],

h_wifi_status:["Wi-Fi Status","Wi-Fi 상태","Wi-Fi 状态","Wi-Fi 状態","WLAN Status","Estado Wi-Fi","État Wi-Fi"],

h_dev_ctrl:["Device Control","기기 제어","设备控制","機器制御","Gerätesteuerung","Control del Dispositivo","Contrôle Appareil"],

h_btn_wifi:["Wi-Fi Settings","Wi-Fi 설정","Wi-Fi 设置","Wi-Fi 設定","WLAN Einstellungen","Ajustes Wi-Fi","Paramètres Wi-Fi"],

h_btn_update:["Firmware Update","펌웨어 업데이트","固件更新","ファームウェア更新","Firmware-Update","Actualizar Firmware","Mise à jour"],

h_btn_manual:["User Manual","사용 설명서","使用手册","取扱説明書","Benutzerhandbuch","Manual de Usuario","Manuel"],

h_btn_exit:["Exit Wi-Fi Mode","Wi-Fi 모드 종료","退出 Wi-Fi 模式","Wi-Fiモード終了","WLAN beenden","Salir Wi-Fi","Quitter Wi-Fi"],

h_not_conn:["Not connected. AP Mode active.","연결 안됨. AP 모드 활성","未连接，AP模式激活","未接続。APモード有効","Nicht verbunden. AP Modus","No conectado. Modo AP","Non connecté. Mode AP"],

h_connected:["Connected","연결됨","已连接","接続済","Verbunden","Conectado","Connecté"],

h_ip_addr:["IP:","IP:","IP:","IP:","IP:","IP:","IP:"],

h_status_err:["Status unavailable","상태 불러오기 실패","状态获取失败","状態取得失敗","Status nicht verfügbar","Estado no disponible","Statut indisponible"],

w_loading:["Loading...","불러오는 중...","加载中...","読み込み中...","Laden...","Cargando...","Chargement..."],

w_cur_status:["Current Wi-Fi Status","현재 Wi-Fi 상태","当前 Wi-Fi 状态","現在のWi-Fi状態","Aktueller WLAN Status","Estado Wi-Fi actual","État Wi-Fi actuel"],

w_conn_card:["Wi-Fi Connection","Wi-Fi 연결","Wi-Fi 连接","Wi-Fi 接続","WLAN-Verbindung","Conexión Wi-Fi","Connexion Wi-Fi"],

w_sel_ssid:["Select SSID:","SSID 선택:","选择 SSID:","SSID 選択:","SSID wählen:","Selec. SSID:","Sélec. SSID:"],

w_scan_ph:["-- Scan to select a network --","-- 스캔 후 선택 --","-- 扫描后选择 --","-- スキャン後選択 --","-- Scannen --","-- Escanear red --","-- Scanner --"],

w_rescan:["Rescan","재스캔","重新扫描","再スキャン","Neu scannen","Reescanear","Rescanner"],

w_pass:["Password:","비밀번호:","密码:","パスワード:","Passwort:","Contraseña:","Mot de passe:"],

w_connect:["Connect","연결","连接","接続","Verbinden","Conectar","Connecter"],

w_disconnect:["Disconnect","연결 해제","断开","切断","Trennen","Desconectar","Déconnecter"],

u_cur_ver:["Current Version:","현재 버전:","当前版本:","現在バージョン:","Aktuelle Version:","Versión actual:","Version actuelle:"],

u_lat_ver:["Latest on Server:","서버 최신:","服务器最新:","サーバー最新:","Neueste Version:","Última en servidor:","Dernière version:"],

u_btn:["Update","업데이트","更新","更新","Aktualisieren","Actualizar","Mettre à jour"],
u_btn_done:["Update Complete","업데이트 완료","更新完成","更新完了","Update fertig","Actualización lista","Mise à jour faite"],


exit_msg:["The device will now return to normal operation. You can close this window.","기기가 일반 모드로 돌아갑니다. 이 창을 닫아도 됩니다.","设备即将返回正常操作，可关闭此窗口","デバイスは通常操作に戻ります","Gerät kehrt in Normalbetrieb zurück","El dispositivo vuelve al modo normal","L'appareil revient en mode normal"],

msg_scanning:["Scanning for Wi-Fi networks...","Wi-Fi 네트워크 스캔 중...","扫描 Wi-Fi 网络...","Wi-Fi検索中...","WLAN suchen...","Buscando redes...","Recherche réseaux..."],

msg_scan_ok:["Scan complete.","스캔 완료","扫描完成","スキャン完了","Scan abgeschlossen","Escaneo completo","Scan terminé"],

msg_connecting:["Attempting to connect...","연결 시도 중...","尝试连接...","接続試行中...","Verbinde...","Intentando conectar...","Connexion en cours..."],

msg_conn_fail:["Connection failed. Check password.","연결 실패. 비밀번호 확인","连接失败，检查密码","接続失敗。パスワード確認","Verbindung fehlgeschlagen","Conexión fallida","Connexion échouée"],

msg_disconnecting:["Disconnecting...","연결 해제 중...","正在断开...","切断中...","Trennen...","Desconectando...","Déconnexion..."],

msg_disconnected:["Disconnected from WiFi.","Wi-Fi 연결 해제됨","已断开 Wi-Fi","Wi-Fi切断","WLAN getrennt","Wi-Fi desconectado","Wi-Fi déconnecté"],

msg_not_conn_s:["Not Connected","미연결","未连接","未接続","Nicht verbunden","No conectado","Non connecté"],

msg_confirm_disc:["Are you sure you want to disconnect?","연결을 끊겠습니까?","确认断开连接？","接続を切断しますか？","Verbindung trennen?","¿Desconectar?","Déconnecter?"],

msg_select_net:["Please select a network first.","먼저 네트워크를 선택하세요","请先选择网络","ネットワークを選択してください","Bitte Netz wählen","Seleccione una red","Sélectionner un réseau"],

msg_sel_net_ph:["-- Select a Network --","-- 네트워크 선택 --","-- 选择网络 --","-- ネットワーク選択 --","-- Netz wählen --","-- Selec. red --","-- Sélec. réseau --"],

msg_up2date:["You are on the latest version.","최신 버전입니다.","已是最新版本","最新バージョンです","Neueste Version vorhanden","Versión más reciente","Version la plus récente"],

msg_update_avail:["Update available!","업데이트 사용 가능!","有可用更新！","アップデートあり！","Update verfügbar!","¡Actualización disponible!","Mise à jour disponible!"],

msg_update_done:["Update Complete! Exit Wi-Fi mode to apply.","업데이트 완료! Wi-Fi 종료 후 적용됩니다.","更新完成！退出Wi-Fi后应用","更新完了！Wi-Fi終了後に適用","Update komplett! WLAN beenden","Actualización completa. Salir Wi-Fi","Mise à jour complète. Quitter Wi-Fi"],

msg_downloading:["Firmware is downloading. Device will reboot when done.","다운로드 중. 완료 후 재부팅됩니다.","固件下载中，完成后重启","DL中。完了後に再起動","Herunterladen... Neustart folgt","Descargando... Reiniciará","Téléchargement en cours..."],

msg_confirm_upd:["Start download? Update applies on Wi-Fi exit.","다운로드 시작? Wi-Fi 모드 종료 시 적용됩니다.","开始下载？退出Wi-Fi后应用","ダウンロード開始？Wi-Fi終了後","Herunterladen? Nach WLAN-Exit","¿Iniciar descarga? Aplica al salir","Lancer le téléchargement?"],

m_s1:["🎮 Button Controls","🎮 버튼 조작법","🎮 按钮操作","🎮 ボタン操作","🎮 Tastenbelegung","🎮 Controles","🎮 Commandes"],

m_s2:["🏠 Home Menu","🏠 홈 메뉴","🏠 主菜单","🏠 ホームメニュー","🏠 Hauptmenü","🏠 Menú Principal","🏠 Menu Principal"],

m_s3:["🎯 EXEC & SETUP","🎯 재생/설정","🎯 执行与设置","🎯 再生＆設定","🎯 AUSF.&EINST.","🎯 EJEC&CONF","🎯 EXEC&CONF"],

m_s4:["💥 GROUP EXEC","💥 그룹 재생","💥 分组执行","💥 グループ再生","💥 GRUPPE","💥 EJEC GRUPO","💥 EXEC GROUPE"],

m_s5:["📡 Auto Channel","📡 자동 채널 선택","📡 自动信道","📡 自動チャネル","📡 Auto-Kanal","📡 Canal Automático","📡 Canal Automatique"],

m_s6:["🔗 Device Pairing","🔗 기기 페어링","🔗 设备配对","🔗 機器ペアリング","🔗 Gerät koppeln","🔗 Emparejamiento","🔗 Appairage"],

m_s7:["📋 Spare Copy","📋 예비 기기 복사","📋 备用复制","📋 予備コピー","📋 Ersatz-Kopie","📋 Copia de Respaldo","📋 Copie de Secours"],

m_s8:["🔄 Firmware Update","🔄 펌웨어 업데이트","🔄 固件更新","🔄 ファームウェア更新","🔄 Firmware-Update","🔄 Actualización Firmware","🔄 Mise à jour Firmware"],

m_s9:["⚙️ Other Settings","⚙️ 기타 설정","⚙️ 其他设置","⚙️ その他の設定","⚙️ Weitere Einstellungen","⚙️ Otros Ajustes","⚙️ Autres Paramètres"],

m_s10:["📊 After Launch Screen","📊 발사 후 화면 보는 법","📊 OLED执行状态","📊 発射後画面の見方","📊 OLED-Status","📊 Estado OLED","📊 État OLED"],

m_s11:["💡 Application Guide","💡 마술 연출 및 응용 가이드","💡 魔术演示与应用指南","💡 マジック演出＆応用ガイド","💡 Magie-Anwendungsleitfaden","💡 Guía de Aplicación Mágica","💡 Guide d'Application Magique"],

msg_checking:["Checking for updates...","업데이트 확인 중...","正在检查更新...","更新を確認中...","Prüfe Updates...","Buscando actualizaciones...","Vérification des mises à jour..."],

msg_check_fail:["Check failed. Make sure Wi-Fi is connected to the internet.","확인 실패. 인터넷이 연결된 Wi-Fi에 접속되어 있는지 확인하세요.","检查失败，请确保Wi-Fi已连接至互联网","確認失敗。インターネット接続を確認してください","Fehler. Bitte WLAN mit Internet verbinden","Error. Compruebe la conexión Wi-Fi a Internet","Échec. Vérifiez la connexion Wi-Fi à Internet"],

msg_retry:["Retry","다시 시도","重试","再試行","Wiederholen","Reintentar","Réessayer"],

msg_connected_ok:["Connected to Wi-Fi!","Wi-Fi 연결됨!","Wi-Fi 已连接！","Wi-Fi 接続完了！","WLAN verbunden!","¡Wi-Fi conectado!","Wi-Fi connecté!"],

msg_wifi_req:["Internet Wi-Fi required. Go to Wi-Fi Settings first.","인터넷 Wi-Fi 연결이 필요합니다. 먼저 Wi-Fi 설정에서 연결하세요.","需要连接互联网Wi-Fi，请先前往Wi-Fi设置。","インターネットWi-Fiが必要です。先にWi-Fi設定で接続してください。","WLAN-Verbindung erforderlich. Bitte zuerst verbinden.","Se requiere Wi-Fi con internet. Ve a Ajustes Wi-Fi primero.","Connexion Wi-Fi internet requise. Allez d'abord dans Paramètres Wi-Fi."],

m_s1_body:["<table class='st'><tr><th>Button</th><th>Action</th></tr><tr><td><span class='kb'>▲ UP</span></td><td>Move up or increase a value</td></tr><tr><td><span class='kb'>▼ DOWN</span></td><td>Move down or decrease a value</td></tr><tr><td><span class='kb'>▶ PLAY</span></td><td>Select / confirm / execute. In TIMER mode a single press fires (▶ on case)</td></tr><tr><td><span class='kb'>▶ PLAY hold</span></td><td>HOLD mode only — device runs while held; stops instantly on release</td></tr><tr><td><span class='kb'>B (BACK)</span></td><td>Go back or cancel editing (B on case)</td></tr><tr><td><span class='kb'>B (BACK) hold</span></td><td>Force-cancel during execution and return to Home</td></tr></table><div class='tb'>💡 Vibration (VIB ON) — TIMER mode: vibrates once when receiver starts, once just before it finishes. No vibration in HOLD mode.</div>","<table class='st'><tr><th>버튼</th><th>하는 일</th></tr><tr><td><span class='kb'>▲ UP</span></td><td>위로 이동하거나 숫자를 올립니다</td></tr><tr><td><span class='kb'>▼ DOWN</span></td><td>아래로 이동하거나 숫자를 내립니다</td></tr><tr><td><span class='kb'>▶ PLAY</span></td><td>선택·확인·실행. TIMER 모드에서 짧게 한 번 누르면 발사됩니다 (케이스에 ▶ 표기)</td></tr><tr><td><span class='kb'>▶ PLAY 길게</span></td><td>HOLD 모드 전용 — 누르는 동안 기기 작동, 손을 떼면 즉시 정지</td></tr><tr><td><span class='kb'>B (BACK)</span></td><td>이전 화면으로 돌아가거나 편집을 취소합니다 (케이스에 B 표기)</td></tr><tr><td><span class='kb'>B (BACK) 길게</span></td><td>실행 도중 강제 취소하고 홈으로 돌아갑니다</td></tr></table><div class='tb'>💡 진동(VIB ON 설정 시) — TIMER 모드: 수신기 작동 시작 시 1회, 완료 직전 1회 진동. HOLD 모드에서는 진동 없음.</div>","<table class='st'><tr><th>按钮</th><th>功能</th></tr><tr><td><span class='kb'>▲ UP</span></td><td>向上移动或增加数值</td></tr><tr><td><span class='kb'>▼ DOWN</span></td><td>向下移动或减少数值</td></tr><tr><td><span class='kb'>▶ PLAY</span></td><td>选择/确认/执行。TIMER 模式下单按即发射（外壳标注 ▶）</td></tr><tr><td><span class='kb'>▶ PLAY 长按</span></td><td>仅限 HOLD 模式 — 按住时设备运行，松开立即停止</td></tr><tr><td><span class='kb'>B (BACK)</span></td><td>返回上一画面或取消编辑（外壳标注 B）</td></tr><tr><td><span class='kb'>B (BACK) 长按</span></td><td>执行中强制取消并返回主菜单</td></tr></table><div class='tb'>💡 振动（VIB ON）— TIMER模式：接收器开始时振动1次，完成前振动1次。HOLD模式无振动。</div>","<table class='st'><tr><th>ボタン</th><th>操作内容</th></tr><tr><td><span class='kb'>▲ UP</span></td><td>上に移動または数値を増加</td></tr><tr><td><span class='kb'>▼ DOWN</span></td><td>下に移動または数値を減少</td></tr><tr><td><span class='kb'>▶ PLAY</span></td><td>選択・確定・実行。TIMERモードで短押しすると発射（ケースに▶表記）</td></tr><tr><td><span class='kb'>▶ PLAY 長押し</span></td><td>HOLDモード専用 — 押している間動作、離すと即停止</td></tr><tr><td><span class='kb'>B (BACK)</span></td><td>前の画面に戻るか編集をキャンセル（ケースにB表記）</td></tr><tr><td><span class='kb'>B (BACK) 長押し</span></td><td>実行中に強制キャンセルしてホームに戻る</td></tr></table><div class='tb'>💡 バイブ（VIB ON時）— TIMERモード：受信機が動作開始時に1回、完了直前に1回振動。HOLDモードは振動なし。</div>","<table class='st'><tr><th>Taste</th><th>Funktion</th></tr><tr><td><span class='kb'>▲ UP</span></td><td>Nach oben oder Wert erhöhen</td></tr><tr><td><span class='kb'>▼ DOWN</span></td><td>Nach unten oder Wert verringern</td></tr><tr><td><span class='kb'>▶ PLAY</span></td><td>Auswählen / Bestätigen / Ausführen. Im TIMER-Modus einmal drücken zum Auslösen (▶ auf dem Gehäuse)</td></tr><tr><td><span class='kb'>▶ PLAY halten</span></td><td>Nur HOLD-Modus — Gerät läuft beim Halten; stoppt sofort beim Loslassen</td></tr><tr><td><span class='kb'>B (BACK)</span></td><td>Zurück oder Bearbeitung abbrechen (B auf dem Gehäuse)</td></tr><tr><td><span class='kb'>B (BACK) halten</span></td><td>Ausführung abbrechen und zum Hauptmenü zurück</td></tr></table><div class='tb'>💡 Vibration (VIB AN) — TIMER-Modus: Vibration beim Start des Empfängers und kurz vor dem Ende. Kein Vibrationssignal im HOLD-Modus.</div>","<table class='st'><tr><th>Botón</th><th>Función</th></tr><tr><td><span class='kb'>▲ UP</span></td><td>Subir o aumentar valor</td></tr><tr><td><span class='kb'>▼ DOWN</span></td><td>Bajar o disminuir valor</td></tr><tr><td><span class='kb'>▶ PLAY</span></td><td>Seleccionar / confirmar / ejecutar. En modo TIMER una pulsación dispara (▶ en la carcasa)</td></tr><tr><td><span class='kb'>▶ PLAY mantener</span></td><td>Solo modo HOLD — funciona mientras se mantiene; se detiene al soltar</td></tr><tr><td><span class='kb'>B (BACK)</span></td><td>Volver o cancelar edición (B en la carcasa)</td></tr><tr><td><span class='kb'>B (BACK) mantener</span></td><td>Cancelar forzosamente y volver al inicio</td></tr></table><div class='tb'>💡 Vibración (VIB ON) — Modo TIMER: vibra al iniciar y justo antes de terminar. Sin vibración en modo HOLD.</div>","<table class='st'><tr><th>Bouton</th><th>Fonction</th></tr><tr><td><span class='kb'>▲ UP</span></td><td>Monter ou augmenter une valeur</td></tr><tr><td><span class='kb'>▼ DOWN</span></td><td>Descendre ou diminuer une valeur</td></tr><tr><td><span class='kb'>▶ PLAY</span></td><td>Sélectionner / confirmer / exécuter. En mode TIMER une pression déclenche (▶ sur le boîtier)</td></tr><tr><td><span class='kb'>▶ PLAY maintenu</span></td><td>Mode HOLD uniquement — fonctionne pendant la pression ; s'arrête à la libération</td></tr><tr><td><span class='kb'>B (BACK)</span></td><td>Retour ou annulation (B sur le boîtier)</td></tr><tr><td><span class='kb'>B (BACK) maintenu</span></td><td>Annulation forcée et retour à l'accueil</td></tr></table><div class='tb'>💡 Vibration (VIB ON) — Mode TIMER : vibre au démarrage et juste avant la fin. Pas de vibration en mode HOLD.</div>"],

m_s2_body:["<p>This screen appears after power-on. Use UP/DOWN to select an item, then press PLAY.</p><table class='st'><tr><th>#</th><th>Item</th><th>Description</th></tr><tr><td>1</td><td>GROUP EXEC</td><td>Send a fire signal to all receivers in the group. Each operates after its own delay</td></tr><tr><td>2</td><td>EXEC &amp; SETUP</td><td>Select and operate a single receiver. Configure ID, steps, delay, play time and power here</td></tr><tr><td>3</td><td>Auto Channel</td><td>Measures channels 1, 6 and 11 then automatically switches both sides to the most stable channel</td></tr><tr><td>4</td><td>Pairing</td><td>Registers this remote's MAC on a receiver. The receiver will only respond to this remote's signals</td></tr><tr><td>5</td><td>Spare Copy</td><td>Spare remote enters Spare Copy → Main remote enters Pairing. Spare copies the main MAC and operates identically</td></tr><tr><td>6</td><td>Update</td><td>Download the latest firmware wirelessly via smartphone (internet Wi-Fi required)</td></tr><tr><td>7</td><td>VIB: ON/OFF</td><td>Press PLAY to toggle instantly. Vibrates when a receiver starts and just before it finishes</td></tr><tr><td>8</td><td>Language</td><td>Change the OLED display language (current language shown in brackets)</td></tr></table>","<p>전원을 켜면 이 화면이 나타납니다. 위·아래 버튼으로 항목을 고른 뒤 PLAY를 누르세요.</p><table class='st'><tr><th>#</th><th>항목</th><th>설명</th></tr><tr><td>1</td><td>그룹 재생 (GROUP EXEC)</td><td>그룹에 속한 수신기 전체에 발사 신호를 전송합니다. 각 수신기는 자신의 대기 시간에 따라 작동합니다</td></tr><tr><td>2</td><td>재생/설정 (EXEC &amp; SETUP)</td><td>원하는 수신기 한 대만 골라서 작동시킵니다. ID·스텝·대기·재생·출력 세기 설정이 모두 이 화면에서 가능합니다</td></tr><tr><td>3</td><td>자동 채널</td><td>채널 1·6·11을 측정해 가장 안정적인 채널을 자동으로 찾고 양쪽을 함께 전환합니다</td></tr><tr><td>4</td><td>페어링</td><td>이 리모컨의 MAC 주소를 수신기에 등록합니다. 수신기는 등록된 리모컨의 신호에만 반응합니다</td></tr><tr><td>5</td><td>예비 복사</td><td>예비 리모컨이 예비 복사 진입 → 메인 리모컨이 페어링 진입. 예비 리모컨이 메인 MAC을 복제해 동일하게 작동합니다</td></tr><tr><td>6</td><td>업데이트</td><td>스마트폰을 통해 최신 펌웨어를 무선으로 내려받습니다 (인터넷 Wi-Fi 필요)</td></tr><tr><td>7</td><td>VIB: ON/OFF</td><td>PLAY를 누르면 즉시 토글. 수신기 작동 시작·완료 직전에 진동으로 알림을 줍니다</td></tr><tr><td>8</td><td>언어</td><td>OLED 화면에 표시되는 언어를 바꿉니다 (현재 언어는 [이름] 형식으로 표시됩니다)</td></tr></table>","<p>开机后显示此画面。用上/下键选择项目，然后按 PLAY。</p><table class='st'><tr><th>#</th><th>项目</th><th>说明</th></tr><tr><td>1</td><td>GROUP EXEC</td><td>向同组所有接收器发送发射信号。每个接收器按各自设定的延迟时间依次动作</td></tr><tr><td>2</td><td>EXEC &amp; SETUP</td><td>选择单个接收器进行操作。可在此设置 ID、步骤、延迟、播放时间和功率</td></tr><tr><td>3</td><td>自动信道</td><td>测量信道 1、6、11，自动找出并切换到最稳定的信道（两端同步切换）</td></tr><tr><td>4</td><td>配对</td><td>将本遥控器的 MAC 地址注册到接收器。接收器仅响应已注册遥控器的信号</td></tr><tr><td>5</td><td>备用复制</td><td>备用遥控进入备用复制 → 主遥控进入配对。备用遥控复制主遥控 MAC，实现相同操作</td></tr><tr><td>6</td><td>更新</td><td>通过智能手机无线下载最新固件（需要联网 Wi-Fi）</td></tr><tr><td>7</td><td>VIB: ON/OFF</td><td>按 PLAY 立即切换。接收器开始和完成前会振动提示</td></tr><tr><td>8</td><td>语言</td><td>更改 OLED 显示语言（当前语言以 [名称] 格式显示）</td></tr></table>","<p>電源投入後にこの画面が表示されます。上下ボタンで項目を選びPLAYを押してください。</p><table class='st'><tr><th>#</th><th>項目</th><th>説明</th></tr><tr><td>1</td><td>GROUP EXEC</td><td>グループ内の全受信機に発射信号を送信。各受信機は自身の遅延設定に従って動作します</td></tr><tr><td>2</td><td>EXEC &amp; SETUP</td><td>受信機を1台選んで操作します。ID・ステップ・遅延・再生時間・出力をここで設定できます</td></tr><tr><td>3</td><td>自動チャネル</td><td>チャネル1・6・11を測定し、最も安定したチャネルを自動選択して両方を切り替えます</td></tr><tr><td>4</td><td>ペアリング</td><td>このリモコンのMACアドレスを受信機に登録します。受信機は登録済みリモコンの信号のみに反応します</td></tr><tr><td>5</td><td>予備コピー</td><td>予備リモコンが予備コピーに進入 → メインリモコンがペアリングに進入。予備がメインのMACを複製して同等に動作します</td></tr><tr><td>6</td><td>更新</td><td>スマートフォンで最新ファームウェアを無線ダウンロード（インターネットWi-Fi必要）</td></tr><tr><td>7</td><td>VIB: ON/OFF</td><td>PLAYを押すと即座にトグル。受信機の動作開始・完了直前に振動で通知します</td></tr><tr><td>8</td><td>言語</td><td>OLED表示言語を変更します（現在の言語は[名称]形式で表示）</td></tr></table>","<p>Dieser Bildschirm erscheint nach dem Einschalten. Mit UP/DOWN auswählen, dann PLAY drücken.</p><table class='st'><tr><th>#</th><th>Punkt</th><th>Beschreibung</th></tr><tr><td>1</td><td>GROUP EXEC</td><td>Sendet ein Auslösesignal an alle Empfänger der Gruppe. Jeder arbeitet nach seiner eigenen Verzögerung</td></tr><tr><td>2</td><td>EXEC &amp; SETUP</td><td>Einen einzelnen Empfänger auswählen und betreiben. ID, Schritte, Verzögerung, Spielzeit und Leistung einstellen</td></tr><tr><td>3</td><td>Auto-Kanal</td><td>Misst Kanäle 1, 6 und 11 und wechselt automatisch zum stabilsten Kanal (beide Seiten gleichzeitig)</td></tr><tr><td>4</td><td>Kopplung</td><td>Registriert die MAC-Adresse dieser Fernbedienung am Empfänger. Der Empfänger reagiert nur auf diese Fernbedienung</td></tr><tr><td>5</td><td>Ersatz-Kopie</td><td>Ersatz-FB in Ersatz-Kopie → Haupt-FB in Kopplung. Ersatz kopiert die MAC der Haupt-FB und arbeitet identisch</td></tr><tr><td>6</td><td>Update</td><td>Neueste Firmware kabellos per Smartphone herunterladen (Internet-WLAN erforderlich)</td></tr><tr><td>7</td><td>VIB: AN/AUS</td><td>PLAY drücken zum sofortigen Umschalten. Vibriert bei Start und kurz vor Ende des Empfängers</td></tr><tr><td>8</td><td>Sprache</td><td>OLED-Anzeigesprache ändern (aktuelle Sprache in Klammern angezeigt)</td></tr></table>","<p>Esta pantalla aparece al encender. Usa UP/DOWN para seleccionar y pulsa PLAY.</p><table class='st'><tr><th>#</th><th>Elemento</th><th>Descripción</th></tr><tr><td>1</td><td>GROUP EXEC</td><td>Envía señal de disparo a todos los receptores del grupo. Cada uno actúa según su propio retardo</td></tr><tr><td>2</td><td>EXEC &amp; SETUP</td><td>Selecciona y opera un solo receptor. Configura ID, pasos, retardo, tiempo de reproducción y potencia</td></tr><tr><td>3</td><td>Canal Automático</td><td>Mide los canales 1, 6 y 11 y cambia automáticamente al más estable (ambos lados a la vez)</td></tr><tr><td>4</td><td>Emparejamiento</td><td>Registra la MAC de este mando en el receptor. El receptor solo responde a este mando</td></tr><tr><td>5</td><td>Copia de Respaldo</td><td>Mando de respaldo entra en Copia → Mando principal entra en Emparejamiento. El respaldo copia la MAC y opera igual</td></tr><tr><td>6</td><td>Actualizar</td><td>Descarga el firmware más reciente por Wi-Fi desde el smartphone (requiere Wi-Fi con internet)</td></tr><tr><td>7</td><td>VIB: ON/OFF</td><td>Pulsa PLAY para alternar. Vibra cuando el receptor inicia y justo antes de terminar</td></tr><tr><td>8</td><td>Idioma</td><td>Cambia el idioma de la pantalla OLED (idioma actual entre corchetes)</td></tr></table>","<p>Cet écran apparaît après la mise sous tension. UP/DOWN pour sélectionner, puis PLAY.</p><table class='st'><tr><th>#</th><th>Élément</th><th>Description</th></tr><tr><td>1</td><td>GROUP EXEC</td><td>Envoie un signal de déclenchement à tous les récepteurs du groupe. Chacun agit selon son propre délai</td></tr><tr><td>2</td><td>EXEC &amp; SETUP</td><td>Sélectionne et actionne un seul récepteur. Configure ID, étapes, délai, durée et puissance</td></tr><tr><td>3</td><td>Canal Auto</td><td>Mesure les canaux 1, 6 et 11 et bascule automatiquement vers le plus stable (les deux côtés simultanément)</td></tr><tr><td>4</td><td>Appairage</td><td>Enregistre l'adresse MAC de cette télécommande sur le récepteur. Celui-ci ne répondra qu'à cette télécommande</td></tr><tr><td>5</td><td>Copie de Secours</td><td>La télécommande de secours entre dans Copie → La principale entre dans Appairage. La secours copie la MAC et fonctionne identiquement</td></tr><tr><td>6</td><td>Mise à jour</td><td>Télécharge le dernier firmware sans fil via smartphone (Wi-Fi internet requis)</td></tr><tr><td>7</td><td>VIB: ON/OFF</td><td>Appuyez sur PLAY pour basculer. Vibre au démarrage et juste avant la fin du récepteur</td></tr><tr><td>8</td><td>Langue</td><td>Changer la langue d'affichage OLED (langue actuelle entre crochets)</td></tr></table>"],

m_s4_body:["<p>Sends a fire signal <b>simultaneously</b> to all receivers in the group. Each receiver operates after <b>its own delay</b> elapses.</p><table class='st'><tr><th>Row</th><th>Item</th><th>Description</th></tr><tr><td>1</td><td>PLAY</td><td>Fire. TIMER: one press sends to all. HOLD: keep-alive packet sent to whole group every 60 ms while held</td></tr><tr><td>2</td><td>STYLE</td><td>Press PLAY to toggle TIMER ↔ HOLD</td></tr><tr><td>3</td><td>GROUP</td><td>Group number (1–5). PLAY to edit → UP/DOWN → PLAY/BACK to confirm</td></tr><tr><td>4</td><td>MEMBERS</td><td>Shows member count of current group. PLAY to enter member edit screen</td></tr></table><table class='st' style='margin-top:10px;'><tr><th>Mode</th><th>How it works</th></tr><tr><td>TIMER</td><td>One PLAY → fire signal sent to all members. Each device activates after its delay</td></tr><tr><td>HOLD</td><td>Keep-alive packet sent to whole group every 60 ms while PLAY is held. Stop signal sent 3× on release</td></tr></table><div class='tb'>💡 MEMBERS edit: UP/DOWN moves between receiver IDs → PLAY toggles include/exclude from current group. Saved instantly.</div><div class='tb'>💡 Set each receiver's delay, play time and power in EXEC &amp; SETUP by ID.</div>","<p>같은 그룹에 속한 수신기 전체에 발사 신호를 <b>동시에</b> 전송합니다. 각 수신기는 <b>자신에게 설정된 대기 시간</b>이 지나야 실제로 작동합니다.</p><table class='st'><tr><th>행</th><th>항목</th><th>설명</th></tr><tr><td>1</td><td>PLAY</td><td>발사. TIMER: 한 번 누르면 전체 전송. HOLD: 누르는 동안 60ms마다 그룹 전체에 킵얼라이브 패킷 전송</td></tr><tr><td>2</td><td>STYLE</td><td>PLAY를 누르면 TIMER ↔ HOLD 전환</td></tr><tr><td>3</td><td>GROUP</td><td>그룹 번호(1~5). PLAY로 편집 진입 → UP/DOWN으로 변경 → PLAY/BACK으로 확인</td></tr><tr><td>4</td><td>MEMBERS</td><td>현재 그룹의 멤버 수 표시. PLAY로 멤버 편집 화면 진입</td></tr></table><table class='st' style='margin-top:10px;'><tr><th>방식</th><th>어떻게 작동하나요?</th></tr><tr><td>TIMER</td><td>PLAY 한 번 → 모든 멤버에 발사 신호 전송. 각 기기는 자신의 대기 시간 후 자동 작동</td></tr><tr><td>HOLD</td><td>PLAY 누르는 동안 60ms마다 킵얼라이브 패킷 전송. 손을 떼면 정지 신호 3회 전송</td></tr></table><div class='tb'>💡 MEMBERS 편집 화면: UP/DOWN으로 수신기 번호 이동 → PLAY로 현재 그룹 포함/제외 토글. 변경 즉시 저장됩니다.</div><div class='tb'>💡 각 수신기의 대기·재생 시간과 출력 세기는 재생/설정(EXEC &amp; SETUP) 메뉴에서 ID별로 설정하세요.</div>","<p>向同组所有接收器<b>同时</b>发送发射信号。每个接收器在<b>各自的等待时间</b>结束后才实际动作。</p><table class='st'><tr><th>行</th><th>项目</th><th>说明</th></tr><tr><td>1</td><td>PLAY</td><td>发射。TIMER：单按向全组发送。HOLD：按住时每60ms向全组发送保活包</td></tr><tr><td>2</td><td>STYLE</td><td>按PLAY切换 TIMER ↔ HOLD</td></tr><tr><td>3</td><td>GROUP</td><td>组编号(1~5)。PLAY进入编辑→上/下更改→PLAY/BACK确认</td></tr><tr><td>4</td><td>MEMBERS</td><td>显示当前组成员数。PLAY进入成员编辑画面</td></tr></table><table class='st' style='margin-top:10px;'><tr><th>模式</th><th>工作方式</th></tr><tr><td>TIMER</td><td>按一次PLAY → 向所有成员发送发射信号。每台设备按各自延迟后自动动作</td></tr><tr><td>HOLD</td><td>按住PLAY时每60ms向全组发送保活包。松开时发送3次停止信号</td></tr></table><div class='tb'>💡 MEMBERS编辑：上/下键移动接收器编号 → PLAY切换加入/移出当前组。立即保存。</div><div class='tb'>💡 在EXEC &amp; SETUP菜单中按ID设置各接收器的延迟、播放时间和功率。</div>","<p>同じグループの全受信機に発射信号を<b>同時に</b>送信します。各受信機は<b>自身の遅延設定</b>が経過してから実際に動作します。</p><table class='st'><tr><th>行</th><th>項目</th><th>説明</th></tr><tr><td>1</td><td>PLAY</td><td>発射。TIMER：1回押して全体送信。HOLD：押している間60msごとにグループ全体にキープアライブ送信</td></tr><tr><td>2</td><td>STYLE</td><td>PLAYを押してTIMER↔HOLD切替</td></tr><tr><td>3</td><td>GROUP</td><td>グループ番号(1~5)。PLAYで編集→上下で変更→PLAY/BACKで確定</td></tr><tr><td>4</td><td>MEMBERS</td><td>現在グループのメンバー数表示。PLAYでメンバー編集画面へ</td></tr></table><table class='st' style='margin-top:10px;'><tr><th>モード</th><th>動作方法</th></tr><tr><td>TIMER</td><td>PLAY 1回 → 全メンバーに発射信号送信。各機器は遅延後自動動作</td></tr><tr><td>HOLD</td><td>PLAY押下中60msごとにキープアライブ送信。離すと停止信号3回送信</td></tr></table><div class='tb'>💡 MEMBERSで編集：上下で受信機番号移動 → PLAYで現在グループへの参加/除外をトグル。即時保存されます。</div><div class='tb'>💡 各受信機の遅延・再生時間・出力はEXEC &amp; SETUPメニューでID別に設定してください。</div>","<p>Sendet gleichzeitig ein Auslösesignal an alle Empfänger der Gruppe. Jeder Empfänger arbeitet erst nach <b>seiner eigenen Verzögerung</b>.</p><table class='st'><tr><th>Zeile</th><th>Punkt</th><th>Beschreibung</th></tr><tr><td>1</td><td>PLAY</td><td>Auslösen. TIMER: einmal drücken sendet an alle. HOLD: Keep-Alive-Paket alle 60 ms an Gruppe, solange gehalten</td></tr><tr><td>2</td><td>STYLE</td><td>PLAY drücken zum Umschalten TIMER ↔ HOLD</td></tr><tr><td>3</td><td>GROUP</td><td>Gruppennummer (1–5). PLAY zum Bearbeiten → UP/DOWN → PLAY/BACK bestätigen</td></tr><tr><td>4</td><td>MEMBERS</td><td>Zeigt Mitgliederanzahl der aktuellen Gruppe. PLAY öffnet Mitglieder-Editor</td></tr></table><table class='st' style='margin-top:10px;'><tr><th>Modus</th><th>Funktionsweise</th></tr><tr><td>TIMER</td><td>1× PLAY → Auslösesignal an alle Mitglieder. Jedes Gerät aktiviert sich nach eigener Verzögerung</td></tr><tr><td>HOLD</td><td>Keep-Alive alle 60 ms während PLAY gehalten. Stoppsignal 3× beim Loslassen</td></tr></table><div class='tb'>💡 MEMBERS-Editor: UP/DOWN zwischen Empfänger-IDs → PLAY zum Ein-/Ausschließen aus der Gruppe. Sofort gespeichert.</div><div class='tb'>💡 Verzögerung, Spielzeit und Leistung jedes Empfängers in EXEC &amp; SETUP nach ID einstellen.</div>","<p>Envía una señal de disparo <b>simultáneamente</b> a todos los receptores del grupo. Cada uno opera tras su <b>propio retardo</b>.</p><table class='st'><tr><th>Fila</th><th>Elemento</th><th>Descripción</th></tr><tr><td>1</td><td>PLAY</td><td>Disparar. TIMER: una pulsación envía a todos. HOLD: paquete keep-alive al grupo cada 60 ms mientras se mantiene</td></tr><tr><td>2</td><td>STYLE</td><td>Pulsa PLAY para alternar TIMER ↔ HOLD</td></tr><tr><td>3</td><td>GROUP</td><td>Número de grupo (1–5). PLAY para editar → UP/DOWN → PLAY/BACK confirmar</td></tr><tr><td>4</td><td>MEMBERS</td><td>Muestra número de miembros del grupo actual. PLAY abre editor de miembros</td></tr></table><table class='st' style='margin-top:10px;'><tr><th>Modo</th><th>Funcionamiento</th></tr><tr><td>TIMER</td><td>1× PLAY → señal enviada a todos. Cada dispositivo actúa tras su retardo</td></tr><tr><td>HOLD</td><td>Keep-alive cada 60 ms mientras se mantiene. Señal de parada 3× al soltar</td></tr></table><div class='tb'>💡 Editor MEMBERS: UP/DOWN entre IDs → PLAY alterna incluir/excluir del grupo. Se guarda al instante.</div><div class='tb'>💡 Configura retardo, tiempo y potencia de cada receptor en EXEC &amp; SETUP por ID.</div>","<p>Envoie un signal de déclenchement <b>simultanément</b> à tous les récepteurs du groupe. Chacun agit après <b>son propre délai</b>.</p><table class='st'><tr><th>Ligne</th><th>Élément</th><th>Description</th></tr><tr><td>1</td><td>PLAY</td><td>Déclencher. TIMER : une pression envoie à tous. HOLD : paquet keep-alive toutes les 60 ms au groupe pendant la pression</td></tr><tr><td>2</td><td>STYLE</td><td>Appuyez sur PLAY pour basculer TIMER ↔ HOLD</td></tr><tr><td>3</td><td>GROUP</td><td>Numéro de groupe (1–5). PLAY pour éditer → UP/DOWN → PLAY/BACK confirmer</td></tr><tr><td>4</td><td>MEMBERS</td><td>Affiche le nombre de membres du groupe actuel. PLAY ouvre l'éditeur de membres</td></tr></table><table class='st' style='margin-top:10px;'><tr><th>Mode</th><th>Fonctionnement</th></tr><tr><td>TIMER</td><td>1× PLAY → signal envoyé à tous. Chaque appareil s'active après son délai</td></tr><tr><td>HOLD</td><td>Keep-alive toutes les 60 ms pendant la pression. Signal d'arrêt 3× au relâchement</td></tr></table><div class='tb'>💡 Éditeur MEMBERS : UP/DOWN entre les IDs → PLAY pour inclure/exclure du groupe. Sauvegardé instantanément.</div><div class='tb'>💡 Configurez délai, durée et puissance de chaque récepteur dans EXEC &amp; SETUP par ID.</div>"],

m_s5_body:["<p>Measures communication success rates on channels 1, 6 and 11, then automatically switches <b>both the remote and all receivers</b> to the most stable channel. Use this when ERR appears frequently or signal is unstable.</p><ol class='sl'><li>Power on <b>all receivers</b> you intend to use</li><li>Home menu → <b>Auto Channel</b> → <span class='kb'>▶</span></li><li>Press <span class='kb'>▶</span> to start scan (takes ~7–12 s)</li><li>After completion, the optimal channel and each channel's success rate are shown</li><li>Channel is <b>applied automatically</b> — press <span class='kb'>B</span> to exit or <span class='kb'>▶</span> to re-scan</li></ol><div class='tb'>✅ Channel switching is bidirectional — the receiver reports back and the remote confirms, so both switch together.</div><div class='tb'>💡 If channel 1 is not more than 20% behind another channel, the current channel (1) is kept. (Stability first)</div><div class='wb'>⚠ Firing is not available during a scan.</div>","<p>채널 1·6·11에서 수신기와의 통신 성공률을 측정한 뒤, 가장 안정적인 채널을 자동으로 찾아 <b>리모컨과 수신기 양쪽을 함께 전환</b>합니다. ERR이 자주 뜨거나 신호가 불안정할 때 사용하세요.</p><ol class='sl'><li>사용할 수신기를 <b>모두 전원 켜두기</b></li><li>홈 메뉴 → <b>자동 채널</b> → <span class='kb'>▶</span></li><li><span class='kb'>▶</span>로 스캔 시작 (약 7~12초 소요)</li><li>완료 후 최적 채널과 각 채널 성공률이 표시됩니다</li><li>채널이 <b>자동으로 적용</b>됩니다 — <span class='kb'>B</span>로 나가거나 <span class='kb'>▶</span>로 재측정</li></ol><div class='tb'>✅ 채널 전환은 양방향 — 수신기가 리포트를 보내고 리모컨이 확정해야 양쪽이 함께 전환됩니다.</div><div class='tb'>💡 채널 1이 다른 채널보다 20% 이상 뒤처지지 않으면 현재 채널(1)을 유지합니다. (안정성 우선)</div><div class='wb'>⚠ 스캔 중에는 발사 기능을 사용할 수 없습니다.</div>","<p>测量信道1、6、11的通信成功率，自动找出最稳定的信道并<b>将遥控器和接收器两端同步切换</b>。当ERR频繁出现或信号不稳定时使用。</p><ol class='sl'><li>开启所有要使用的接收器电源</li><li>主菜单 → <b>自动信道</b> → <span class='kb'>▶</span></li><li>按<span class='kb'>▶</span>开始扫描（约7~12秒）</li><li>完成后显示最佳信道及各信道成功率</li><li>信道<b>自动应用</b> — 按<span class='kb'>B</span>退出或<span class='kb'>▶</span>重新测量</li></ol><div class='tb'>✅ 信道切换是双向的 — 接收器上报，遥控器确认，双端同步切换。</div><div class='tb'>💡 若信道1的成功率不低于其他信道20%以上，则保持当前信道（稳定性优先）。</div><div class='wb'>⚠ 扫描期间无法使用发射功能。</div>","<p>チャネル1・6・11の通信成功率を測定し、最も安定したチャネルを自動選択して<b>リモコンと受信機の両方を同時に切り替え</b>ます。ERRが頻繁に出るときや信号が不安定なときに使用してください。</p><ol class='sl'><li>使用する受信機を<b>すべて電源ON</b>にしておく</li><li>ホームメニュー → <b>自動チャネル</b> → <span class='kb'>▶</span></li><li><span class='kb'>▶</span>でスキャン開始（約7〜12秒）</li><li>完了後、最適チャネルと各チャネルの成功率が表示されます</li><li>チャネルが<b>自動で適用</b>されます — <span class='kb'>B</span>で終了、<span class='kb'>▶</span>で再測定</li></ol><div class='tb'>✅ チャネル切替は双方向 — 受信機がレポートを送り、リモコンが確定することで両方が切り替わります。</div><div class='tb'>💡 チャネル1が他のチャネルより20%以上劣っていない場合は現在のチャネル(1)を維持します。（安定性優先）</div><div class='wb'>⚠ スキャン中は発射機能を使用できません。</div>","<p>Misst die Kommunikationserfolgsrate auf Kanälen 1, 6 und 11 und wechselt automatisch <b>sowohl Fernbedienung als auch alle Empfänger</b> zum stabilsten Kanal. Verwenden Sie dies bei häufigem ERR oder instabilem Signal.</p><ol class='sl'><li>Alle zu verwendenden Empfänger <b>einschalten</b></li><li>Hauptmenü → <b>Auto-Kanal</b> → <span class='kb'>▶</span></li><li><span class='kb'>▶</span> drücken zum Starten des Scans (~7–12 s)</li><li>Nach Abschluss werden optimaler Kanal und Erfolgsraten angezeigt</li><li>Kanal wird <b>automatisch angewandt</b> — <span class='kb'>B</span> zum Beenden, <span class='kb'>▶</span> zum Wiederholen</li></ol><div class='tb'>✅ Kanalwechsel ist bidirektional — der Empfänger meldet zurück und die Fernbedienung bestätigt, sodass beide wechseln.</div><div class='tb'>💡 Liegt Kanal 1 nicht mehr als 20 % hinter einem anderen Kanal, wird der aktuelle Kanal (1) beibehalten. (Stabilität zuerst)</div><div class='wb'>⚠ Die Auslösefunktion ist während eines Scans nicht verfügbar.</div>","<p>Mide las tasas de éxito de comunicación en los canales 1, 6 y 11, luego cambia automáticamente <b>tanto el mando como todos los receptores</b> al canal más estable. Úsalo cuando ERR aparezca con frecuencia o la señal sea inestable.</p><ol class='sl'><li>Enciende <b>todos los receptores</b> que vayas a usar</li><li>Menú principal → <b>Canal Automático</b> → <span class='kb'>▶</span></li><li>Pulsa <span class='kb'>▶</span> para iniciar el escaneo (~7–12 s)</li><li>Al finalizar se muestran el canal óptimo y las tasas de éxito</li><li>El canal se <b>aplica automáticamente</b> — pulsa <span class='kb'>B</span> para salir o <span class='kb'>▶</span> para reescanear</li></ol><div class='tb'>✅ El cambio de canal es bidireccional — el receptor informa y el mando confirma, cambiando ambos juntos.</div><div class='tb'>💡 Si el canal 1 no está más de un 20% por detrás de otro, se mantiene el canal actual (1). (Estabilidad primero)</div><div class='wb'>⚠ La función de disparo no está disponible durante el escaneo.</div>","<p>Mesure les taux de succès de communication sur les canaux 1, 6 et 11, puis bascule automatiquement <b>la télécommande et tous les récepteurs</b> vers le canal le plus stable. Utilisez ceci quand ERR apparaît fréquemment ou que le signal est instable.</p><ol class='sl'><li>Allumer <b>tous les récepteurs</b> à utiliser</li><li>Menu principal → <b>Canal Auto</b> → <span class='kb'>▶</span></li><li>Appuyer sur <span class='kb'>▶</span> pour lancer le scan (~7–12 s)</li><li>À la fin, le canal optimal et les taux de succès sont affichés</li><li>Le canal est <b>appliqué automatiquement</b> — <span class='kb'>B</span> pour quitter, <span class='kb'>▶</span> pour relancer</li></ol><div class='tb'>✅ Le changement de canal est bidirectionnel — le récepteur rapporte et la télécommande confirme, les deux basculent ensemble.</div><div class='tb'>💡 Si le canal 1 n'est pas à plus de 20% derrière un autre, le canal actuel (1) est conservé. (Stabilité d'abord)</div><div class='wb'>⚠ La fonction de déclenchement n'est pas disponible pendant un scan.</div>"],

m_s6_body:["<p>The receiver <b>registers this remote's MAC address as its master</b>. After pairing, the receiver only responds to this remote's signals. <b>Both the receiver and remote</b> require action.</p><p style='margin-top:12px;margin-bottom:4px;'><b>① Receiver first</b></p><ol class='sl'><li>Hold the ID button to enter ID SET mode (LED lights for 1 s)</li><li>If ID change needed: short press ID to cycle → long press to confirm (LED blinks that many times)</li><li>Press EXEC button → enter pairing wait mode (LED fast-blinks, 30-s timeout)</li></ol><p style='margin-top:12px;margin-bottom:4px;'><b>② Then the remote</b></p><ol class='sl'><li>Home menu → <b>Pairing</b> → <span class='kb'>▶</span></li><li>Screen shows \"Pairing ON\" — MAC address is broadcast automatically every 1.5 s</li><li>When the receiver LED turns off, pairing is complete → press <span class='kb'>B</span> to exit</li></ol><div class='tb'>💡 Group membership is set in GROUP EXEC → MEMBERS edit screen. This is separate from pairing.</div><div class='tb'>💡 Both sides reset to channel 1 on pairing entry. Run Auto Channel again after pairing.</div><div class='wb'>⚠ Keep only one receiver in pairing mode at a time. Multiple receivers in pairing mode simultaneously may register to the wrong device.</div>","<p>수신기가 <b>이 리모컨의 MAC 주소를 마스터로 등록</b>하는 과정입니다. 등록 후 수신기는 이 리모컨의 신호에만 반응합니다. <b>수신기와 리모컨 양쪽 모두</b>에서 조작이 필요합니다.</p><p style='margin-top:12px;margin-bottom:4px;'><b>① 수신기 조작 (먼저)</b></p><ol class='sl'><li>ID 버튼을 <b>길게</b> 눌러 ID SET 모드 진입 (LED 1초 점등)</li><li>ID 변경 필요 시: ID 버튼 <b>짧게</b> 눌러 번호 이동 → <b>길게</b> 눌러 확정 (LED가 해당 숫자만큼 깜빡임)</li><li>EXEC 버튼 → 페어링 대기 모드 진입 (LED 빠른 깜빡임, 30초 타임아웃)</li></ol><p style='margin-top:12px;margin-bottom:4px;'><b>② 리모컨 조작</b></p><ol class='sl'><li>홈 메뉴 → <b>페어링</b> → <span class='kb'>▶</span></li><li>화면에 \"페어링 신호 ON\" 표시 — MAC 주소를 1.5초 간격으로 자동 브로드캐스트</li><li>수신기 LED가 꺼지면 페어링 완료 → <span class='kb'>B</span>로 나가세요</li></ol><div class='tb'>💡 수신기의 그룹 소속은 그룹 재생 → MEMBERS 편집 화면에서 설정합니다. 페어링과 별개입니다.</div><div class='tb'>💡 페어링 진입 시 리모컨과 수신기 모두 채널 1로 리셋됩니다. 페어링 완료 후에는 <b>자동 채널</b>을 다시 실행하세요.</div><div class='wb'>⚠ 한 번에 수신기 한 대만 페어링 모드로 켜두세요. 여러 대가 동시에 페어링 모드면 엉뚱한 기기에 등록될 수 있습니다.</div>","<p>接收器将<b>本遥控器的MAC地址注册为主控</b>。注册后，接收器仅响应本遥控器的信号。<b>接收器和遥控器双方</b>均需操作。</p><p style='margin-top:12px;margin-bottom:4px;'><b>① 先操作接收器</b></p><ol class='sl'><li>长按ID按钮进入ID SET模式（LED亮1秒）</li><li>如需更改ID：短按ID按钮循环选择 → 长按确定（LED闪烁相应次数）</li><li>按EXEC按钮 → 进入配对等待模式（LED快速闪烁，30秒超时）</li></ol><p style='margin-top:12px;margin-bottom:4px;'><b>② 再操作遥控器</b></p><ol class='sl'><li>主菜单 → <b>配对</b> → <span class='kb'>▶</span></li><li>画面显示\"配对信号ON\" — 每1.5秒自动广播MAC地址</li><li>接收器LED熄灭时配对完成 → 按<span class='kb'>B</span>退出</li></ol><div class='tb'>💡 接收器的组归属在GROUP EXEC → MEMBERS编辑画面中设置，与配对流程无关。</div><div class='tb'>💡 进入配对时遥控器和接收器均重置为信道1。配对完成后请重新运行自动信道。</div><div class='wb'>⚠ 一次只让一台接收器进入配对模式。多台同时配对可能导致错误注册。</div>","<p>受信機が<b>このリモコンのMACアドレスをマスターとして登録</b>します。登録後、受信機はこのリモコンの信号のみに反応します。<b>受信機とリモコン両方</b>での操作が必要です。</p><p style='margin-top:12px;margin-bottom:4px;'><b>① 受信機の操作（先に）</b></p><ol class='sl'><li>IDボタンを<b>長押し</b>してID SETモードへ（LED 1秒点灯）</li><li>ID変更が必要な場合：IDボタン<b>短押し</b>で番号移動 → <b>長押し</b>で確定（LEDがその数だけ点滅）</li><li>EXECボタン → ペアリング待機モードへ（LED高速点滅、30秒タイムアウト）</li></ol><p style='margin-top:12px;margin-bottom:4px;'><b>② リモコンの操作</b></p><ol class='sl'><li>ホームメニュー → <b>ペアリング</b> → <span class='kb'>▶</span></li><li>画面に「ペアリング信号ON」表示 — MACアドレスを1.5秒ごとに自動ブロードキャスト</li><li>受信機LEDが消えたらペアリング完了 → <span class='kb'>B</span>で終了</li></ol><div class='tb'>💡 グループ所属はGROUP EXEC → MEMBERSで設定します。ペアリングとは別です。</div><div class='tb'>💡 ペアリング進入時はリモコン・受信機ともにチャネル1にリセットされます。完了後は自動チャネルを再実行してください。</div><div class='wb'>⚠ 一度に1台の受信機だけをペアリングモードにしてください。複数台同時にペアリングモードにすると誤登録の原因になります。</div>","<p>Der Empfänger <b>registriert die MAC-Adresse dieser Fernbedienung als Master</b>. Nach der Kopplung reagiert der Empfänger nur auf Signale dieser Fernbedienung. <b>Sowohl Empfänger als auch Fernbedienung</b> müssen bedient werden.</p><p style='margin-top:12px;margin-bottom:4px;'><b>① Zuerst Empfänger</b></p><ol class='sl'><li>ID-Taste <b>gehalten</b> drücken für ID-SET-Modus (LED leuchtet 1 s)</li><li>Wenn ID-Änderung nötig: ID-Taste <b>kurz</b> für nächste Nummer → <b>lang</b> zum Bestätigen (LED blinkt entsprechend)</li><li>EXEC-Taste → Kopplungswartemodus (LED blinkt schnell, 30 s Timeout)</li></ol><p style='margin-top:12px;margin-bottom:4px;'><b>② Dann Fernbedienung</b></p><ol class='sl'><li>Hauptmenü → <b>Kopplung</b> → <span class='kb'>▶</span></li><li>Bildschirm zeigt „Kopplung EIN\" — MAC-Adresse wird alle 1,5 s automatisch gesendet</li><li>Wenn die Empfänger-LED erlischt, ist Kopplung abgeschlossen → <span class='kb'>B</span> drücken</li></ol><div class='tb'>💡 Gruppenszugehörigkeit wird in GROUP EXEC → MEMBERS eingestellt. Dies ist unabhängig von der Kopplung.</div><div class='tb'>💡 Beim Eintreten in die Kopplung werden beide auf Kanal 1 zurückgesetzt. Führen Sie Auto-Kanal danach erneut aus.</div><div class='wb'>⚠ Halten Sie immer nur einen Empfänger im Kopplungsmodus. Mehrere gleichzeitig können zu Fehlregistrierungen führen.</div>","<p>El receptor <b>registra la MAC de este mando como su maestro</b>. Tras el emparejamiento, el receptor solo responde a las señales de este mando. Se necesita acción en <b>ambos lados</b>.</p><p style='margin-top:12px;margin-bottom:4px;'><b>① Primero el receptor</b></p><ol class='sl'><li>Manten pulsado el botón ID para entrar en modo ID SET (LED encendido 1 s)</li><li>Si es necesario cambiar ID: pulsación corta en ID para ciclar → larga para confirmar (LED parpadea ese número de veces)</li><li>Botón EXEC → modo espera de emparejamiento (LED parpadea rápido, timeout 30 s)</li></ol><p style='margin-top:12px;margin-bottom:4px;'><b>② Luego el mando</b></p><ol class='sl'><li>Menú principal → <b>Emparejamiento</b> → <span class='kb'>▶</span></li><li>Pantalla muestra \"Señal Emparejamiento ON\" — MAC se difunde automáticamente cada 1,5 s</li><li>Cuando el LED del receptor se apaga, el emparejamiento está completo → pulsa <span class='kb'>B</span> para salir</li></ol><div class='tb'>💡 La membresía de grupo se configura en GROUP EXEC → MEMBERS. Es independiente del emparejamiento.</div><div class='tb'>💡 Al entrar en emparejamiento, ambos lados se reinician al canal 1. Ejecuta Canal Automático de nuevo tras emparejar.</div><div class='wb'>⚠ Mantén solo un receptor en modo emparejamiento a la vez. Varios simultáneos pueden registrarse en el dispositivo incorrecto.</div>","<p>Le récepteur <b>enregistre l'adresse MAC de cette télécommande comme maître</b>. Après l'appairage, le récepteur ne répond qu'aux signaux de cette télécommande. Une action est nécessaire des <b>deux côtés</b>.</p><p style='margin-top:12px;margin-bottom:4px;'><b>① D'abord le récepteur</b></p><ol class='sl'><li>Maintenir le bouton ID pour entrer en mode ID SET (LED allumée 1 s)</li><li>Si changement d'ID nécessaire : courte pression ID pour cycler → longue pour confirmer (LED clignote autant de fois)</li><li>Bouton EXEC → mode attente d'appairage (LED clignote rapidement, timeout 30 s)</li></ol><p style='margin-top:12px;margin-bottom:4px;'><b>② Puis la télécommande</b></p><ol class='sl'><li>Menu principal → <b>Appairage</b> → <span class='kb'>▶</span></li><li>L'écran affiche «Signal Appairage ON» — l'adresse MAC est diffusée automatiquement toutes les 1,5 s</li><li>Quand la LED du récepteur s'éteint, l'appairage est terminé → appuyez sur <span class='kb'>B</span></li></ol><div class='tb'>💡 L'appartenance au groupe se configure dans GROUP EXEC → MEMBERS. C'est indépendant de l'appairage.</div><div class='tb'>💡 À l'entrée en appairage, les deux côtés repassent au canal 1. Relancez le Canal Auto après l'appairage.</div><div class='wb'>⚠ Ne laissez qu'un seul récepteur en mode appairage à la fois. Plusieurs simultanément peuvent provoquer des enregistrements incorrects.</div>"],

m_s7_body:["<p>Clones the <b>main remote's MAC address onto a spare remote</b>. After copying, the spare remote is recognized as the same device, and can control all previously paired receivers without re-pairing.</p><ol class='sl'><li>Power on both main and spare remotes</li><li><b>Spare remote</b>: Home → <b>Spare Copy</b> → <span class='kb'>▶</span> → screen shows \"Waiting for MAIN...\"</li><li><b>Main remote</b>: Home → <b>Pairing</b> → <span class='kb'>▶</span> → MAC address broadcast starts automatically</li><li>When spare shows \"COPY SUCCESS!\" and <b>reboots automatically</b>, copy is complete</li></ol><div class='tb'>✅ After reboot the spare remote operates with the same MAC as the main — no re-pairing of receivers needed.</div><div class='wb'>⚠ To reset the copy: on the spare remote go to Home → Spare Copy → PLAY to enter, then press UP. It reboots as an independent device.</div>","<p>메인 리모컨의 <b>MAC 주소를 예비 리모컨에 그대로 복제</b>하는 기능입니다. 복사 후 예비 리모컨은 메인과 동일한 장치로 인식되어, 기존에 페어링된 수신기를 별도 재페어링 없이 제어할 수 있습니다.</p><ol class='sl'><li>메인·예비 리모컨 모두 전원 켜기</li><li><b>예비 리모컨</b>: 홈 메뉴 → <b>예비 복사</b> → <span class='kb'>▶</span> → 화면에 \"MAIN 대기중...\" 표시</li><li><b>메인 리모컨</b>: 홈 메뉴 → <b>페어링</b> → <span class='kb'>▶</span> → MAC 주소 자동 송신 시작</li><li>예비 리모컨이 \"COPY SUCCESS!\" 표시 후 <b>자동 재부팅</b>되면 완료</li></ol><div class='tb'>✅ 재부팅 후 예비 리모컨은 메인과 동일한 MAC으로 동작 — 수신기 재페어링 불필요.</div><div class='wb'>⚠ 복사를 초기화하려면: 예비 리모컨에서 홈 → 예비 복사 → PLAY로 진입 후, 해당 화면에서 UP 버튼을 누르세요. 재부팅 후 독립 기기로 되돌아갑니다.</div>","<p>将<b>主遥控器的MAC地址完整复制到备用遥控器</b>。复制后备用遥控器被识别为与主遥控器相同的设备，无需重新配对即可控制已配对的接收器。</p><ol class='sl'><li>同时开启主遥控器和备用遥控器</li><li><b>备用遥控器</b>：主菜单 → <b>备用复制</b> → <span class='kb'>▶</span> → 画面显示\"等待MAIN...\"</li><li><b>主遥控器</b>：主菜单 → <b>配对</b> → <span class='kb'>▶</span> → 自动开始发送MAC地址</li><li>备用遥控器显示\"COPY SUCCESS!\"并<b>自动重启</b>后即完成</li></ol><div class='tb'>✅ 重启后备用遥控器以与主遥控器相同的MAC工作 — 无需重新配对接收器。</div><div class='wb'>⚠ 要重置复制：在备用遥控器上进入主菜单 → 备用复制 → PLAY进入，然后按UP键。重启后恢复为独立设备。</div>","<p>メインリモコンの<b>MACアドレスを予備リモコンにそのまま複製</b>する機能です。複製後、予備リモコンはメインと同一デバイスとして認識され、既存のペアリング済み受信機を再ペアリングなしで制御できます。</p><ol class='sl'><li>メイン・予備の両リモコンの電源を入れる</li><li><b>予備リモコン</b>：ホームメニュー → <b>予備コピー</b> → <span class='kb'>▶</span> → 画面に「MAIN待機中...」表示</li><li><b>メインリモコン</b>：ホームメニュー → <b>ペアリング</b> → <span class='kb'>▶</span> → MACアドレスの自動送信開始</li><li>予備リモコンが「COPY SUCCESS!」を表示して<b>自動再起動</b>したら完了</li></ol><div class='tb'>✅ 再起動後、予備リモコンはメインと同じMACで動作 — 受信機の再ペアリング不要。</div><div class='wb'>⚠ 複製をリセットするには：予備リモコンでホーム → 予備コピー → PLAYで進入後、UPボタンを押してください。再起動で独立デバイスに戻ります。</div>","<p>Klont die <b>MAC-Adresse der Haupt-Fernbedienung auf die Ersatz-Fernbedienung</b>. Nach dem Kopieren wird die Ersatz-FB als dasselbe Gerät erkannt und kann alle bereits gekoppelten Empfänger ohne erneute Kopplung steuern.</p><ol class='sl'><li>Haupt- und Ersatz-Fernbedienung einschalten</li><li><b>Ersatz-FB</b>: Hauptmenü → <b>Ersatz-Kopie</b> → <span class='kb'>▶</span> → Bildschirm zeigt \"Warte auf MAIN...\"</li><li><b>Haupt-FB</b>: Hauptmenü → <b>Kopplung</b> → <span class='kb'>▶</span> → MAC-Adresse wird automatisch gesendet</li><li>Wenn Ersatz-FB \"COPY SUCCESS!\" zeigt und sich <b>automatisch neu startet</b>, ist der Kopiervorgang abgeschlossen</li></ol><div class='tb'>✅ Nach dem Neustart arbeitet die Ersatz-FB mit derselben MAC wie die Haupt-FB — keine erneute Empfängerkopplung nötig.</div><div class='wb'>⚠ Zum Zurücksetzen: Auf der Ersatz-FB Hauptmenü → Ersatz-Kopie → PLAY, dann UP drücken. Nach Neustart wieder eigenständiges Gerät.</div>","<p>Clona la <b>dirección MAC del mando principal en el mando de respaldo</b>. Tras la copia, el mando de respaldo se reconoce como el mismo dispositivo y puede controlar todos los receptores ya emparejados sin reemparejar.</p><ol class='sl'><li>Enciende tanto el mando principal como el de respaldo</li><li><b>Mando de respaldo</b>: Menú → <b>Copia de Respaldo</b> → <span class='kb'>▶</span> → pantalla muestra \"Esperando MAIN...\"</li><li><b>Mando principal</b>: Menú → <b>Emparejamiento</b> → <span class='kb'>▶</span> → emisión automática de MAC</li><li>Cuando el respaldo muestra \"COPY SUCCESS!\" y <b>se reinicia automáticamente</b>, la copia está completa</li></ol><div class='tb'>✅ Tras el reinicio, el mando de respaldo funciona con la misma MAC que el principal — no es necesario reemparejar receptores.</div><div class='wb'>⚠ Para resetear la copia: en el mando de respaldo ve a Menú → Copia de Respaldo → PLAY, luego pulsa UP. Reiniciará como dispositivo independiente.</div>","<p>Clone <b>l'adresse MAC de la télécommande principale sur la télécommande de secours</b>. Après la copie, la secours est reconnue comme le même appareil et peut contrôler tous les récepteurs déjà appairés sans re-appairage.</p><ol class='sl'><li>Allumer la télécommande principale et celle de secours</li><li><b>Télécommande de secours</b> : Menu → <b>Copie de Secours</b> → <span class='kb'>▶</span> → écran affiche «En attente du MAIN...»</li><li><b>Télécommande principale</b> : Menu → <b>Appairage</b> → <span class='kb'>▶</span> → diffusion automatique de la MAC</li><li>Quand la secours affiche «COPY SUCCESS!» et <b>redémarre automatiquement</b>, la copie est terminée</li></ol><div class='tb'>✅ Après redémarrage, la secours fonctionne avec la même MAC que la principale — pas besoin de re-appairer les récepteurs.</div><div class='wb'>⚠ Pour réinitialiser la copie : sur la secours, aller dans Menu → Copie de Secours → PLAY, puis appuyer sur UP. Redémarre en appareil indépendant.</div>"],

m_s8_body:["<p>Upgrades the device's software to the latest version using your smartphone. The page you are viewing now is the update screen!</p><ol class='sl'><li>Home menu → <b>Update</b> → <span class='kb'>▶</span></li><li>The device screen shows a Wi-Fi name and address (192.168.4.1)</li><li>Connect your smartphone or tablet to the Wi-Fi network shown on the device screen</li><li>Open a browser (Chrome, Safari, etc.) and enter <b>192.168.4.1</b></li><li>In <b>Wi-Fi Settings</b>, connect to an internet-enabled Wi-Fi network (required — update is disabled until connected)</li><li>On the <b>Firmware Update</b> screen, check for the new version → press Update</li><li>After download completes, click <b>Exit Wi-Fi Mode</b></li><li>The device restarts automatically and the new version is applied</li></ol><div class='tb'>💡 Do not cut power during download. The device only restarts after you press Exit.</div>","<p>스마트폰을 이용해 기기의 소프트웨어를 최신 버전으로 업그레이드합니다. 지금 보고 계신 이 페이지가 바로 업데이트 화면입니다!</p><ol class='sl'><li>홈 메뉴 → <b>업데이트</b> → <span class='kb'>▶</span></li><li>기기 화면에 Wi-Fi 이름과 주소(192.168.4.1)가 나타납니다</li><li>스마트폰이나 태블릿의 Wi-Fi를 기기 화면에 나온 이름으로 연결하세요</li><li>브라우저(크롬, 사파리 등)에서 <b>192.168.4.1</b> 입력 후 접속</li><li><b>Wi-Fi 설정</b>에서 인터넷이 되는 일반 Wi-Fi에 연결 (필수 — 연결 전엔 업데이트 비활성화)</li><li><b>펌웨어 업데이트</b> 화면에서 새 버전 확인 → 업데이트 버튼</li><li>다운로드 완료 후 <b>Wi-Fi 모드 종료</b> 클릭</li><li>기기가 알아서 재시작되고 새 버전이 적용됩니다</li></ol><div class='tb'>💡 다운로드 중에는 절대 전원을 끄지 마세요. 다운로드가 끝난 뒤 종료를 눌러야 재시작됩니다.</div>","<p>通过智能手机将设备软件升级到最新版本。您现在看到的这个页面就是更新画面！</p><ol class='sl'><li>主菜单 → <b>更新</b> → <span class='kb'>▶</span></li><li>设备屏幕显示Wi-Fi名称和地址(192.168.4.1)</li><li>将手机或平板连接到设备屏幕上显示的Wi-Fi</li><li>打开浏览器（Chrome、Safari等）输入 <b>192.168.4.1</b> 后访问</li><li>在<b>Wi-Fi设置</b>中连接可上网的Wi-Fi（必须 — 连接前更新不可用）</li><li>在<b>固件更新</b>画面中确认新版本 → 点击更新按钮</li><li>下载完成后点击<b>退出Wi-Fi模式</b></li><li>设备自动重启并应用新版本</li></ol><div class='tb'>💡 下载过程中请勿断电。下载完成后按退出才会重启。</div>","<p>スマートフォンを使ってデバイスのソフトウェアを最新バージョンにアップグレードします。今ご覧のこのページがアップデート画面です！</p><ol class='sl'><li>ホームメニュー → <b>更新</b> → <span class='kb'>▶</span></li><li>デバイス画面にWi-Fi名とアドレス(192.168.4.1)が表示されます</li><li>スマートフォンやタブレットのWi-Fiをデバイス画面に表示されたWi-Fiに接続してください</li><li>ブラウザ（Chrome、Safariなど）で <b>192.168.4.1</b> を入力してアクセス</li><li><b>Wi-Fi設定</b>でインターネット接続できるWi-Fiに接続（必須 — 接続前はアップデート無効）</li><li><b>ファームウェアアップデート</b>画面で新バージョン確認 → アップデートボタン</li><li>ダウンロード完了後、<b>Wi-Fiモード終了</b>をクリック</li><li>デバイスが自動再起動して新バージョンが適用されます</li></ol><div class='tb'>💡 ダウンロード中は絶対に電源を切らないでください。ダウンロード完了後に終了を押してから再起動されます。</div>","<p>Aktualisiert die Gerätesoftware mit dem Smartphone auf die neueste Version. Die Seite, die Sie gerade sehen, ist der Update-Bildschirm!</p><ol class='sl'><li>Hauptmenü → <b>Update</b> → <span class='kb'>▶</span></li><li>Der Gerätebildschirm zeigt WLAN-Name und Adresse (192.168.4.1)</li><li>Verbinden Sie Smartphone oder Tablet mit dem auf dem Gerät angezeigten WLAN</li><li>Browser öffnen (Chrome, Safari usw.) und <b>192.168.4.1</b> eingeben</li><li>In <b>WLAN-Einstellungen</b> mit einem internetfähigen WLAN verbinden (erforderlich — Update deaktiviert bis verbunden)</li><li>Auf dem <b>Firmware-Update</b>-Bildschirm neue Version prüfen → Update-Taste drücken</li><li>Nach Abschluss des Downloads auf <b>WLAN-Modus beenden</b> klicken</li><li>Das Gerät startet automatisch neu und die neue Version wird angewendet</li></ol><div class='tb'>💡 Trennen Sie niemals die Stromversorgung während des Downloads. Das Gerät startet erst nach dem Drücken von Beenden neu.</div>","<p>Actualiza el software del dispositivo a la última versión usando tu smartphone. ¡La página que estás viendo ahora mismo es la pantalla de actualización!</p><ol class='sl'><li>Menú principal → <b>Actualizar</b> → <span class='kb'>▶</span></li><li>La pantalla del dispositivo muestra el nombre Wi-Fi y la dirección (192.168.4.1)</li><li>Conecta tu smartphone o tablet al Wi-Fi que aparece en la pantalla del dispositivo</li><li>Abre un navegador (Chrome, Safari, etc.) e introduce <b>192.168.4.1</b></li><li>En <b>Ajustes Wi-Fi</b>, conéctate a un Wi-Fi con internet (obligatorio — la actualización está desactivada hasta conectarse)</li><li>En la pantalla de <b>Actualización de Firmware</b>, comprueba la nueva versión → pulsa Actualizar</li><li>Cuando termine la descarga, pulsa <b>Salir del Modo Wi-Fi</b></li><li>El dispositivo se reinicia automáticamente y se aplica la nueva versión</li></ol><div class='tb'>💡 No cortes la alimentación durante la descarga. El dispositivo solo se reinicia después de pulsar Salir.</div>","<p>Met à jour le logiciel de l'appareil vers la dernière version via votre smartphone. La page que vous regardez en ce moment est l'écran de mise à jour !</p><ol class='sl'><li>Menu principal → <b>Mise à jour</b> → <span class='kb'>▶</span></li><li>L'écran de l'appareil affiche le nom Wi-Fi et l'adresse (192.168.4.1)</li><li>Connectez votre smartphone ou tablette au Wi-Fi affiché sur l'écran de l'appareil</li><li>Ouvrez un navigateur (Chrome, Safari, etc.) et entrez <b>192.168.4.1</b></li><li>Dans <b>Paramètres Wi-Fi</b>, connectez-vous à un Wi-Fi avec internet (obligatoire — mise à jour désactivée jusqu'à la connexion)</li><li>Sur l'écran <b>Mise à jour Firmware</b>, vérifiez la nouvelle version → appuyez sur Mettre à jour</li><li>Après le téléchargement, cliquez sur <b>Quitter le Mode Wi-Fi</b></li><li>L'appareil redémarre automatiquement et la nouvelle version est appliquée</li></ol><div class='tb'>💡 Ne coupez jamais l'alimentation pendant le téléchargement. L'appareil ne redémarre qu'après avoir appuyé sur Quitter.</div>"],

m_s9_body:["<p><b>Vibration alert (VIB)</b><br>Select <span class='kb'>VIB: ON/OFF</span> in the Home menu and press PLAY to toggle instantly.</p><p>When VIB is ON, vibration timing:</p><table class='st'><tr><th style='text-align:left'>Timing</th><th>Description</th></tr><tr><td style='text-align:left'>Receiver starts</td><td>Vibrates once when the delay elapses and the receiver actually starts operating</td></tr><tr><td style='text-align:left'>Receiver nearly done</td><td>Vibrates once ~0.5 s before the end of operation (advance notice)</td></tr></table><div class='wb'>⚠ No vibration in HOLD mode.</div><p style='margin-top:14px;'><b>Change display language</b><br>Home menu → <span class='kb'>Language</span> → PLAY. Use UP/DOWN to pick a language, then press PLAY to apply immediately. The selected language is shown in <b>[brackets]</b>.</p><div class='tb'>💡 The web page language can be changed with the language button (EN / 한국어 / 中文 etc.) at the top of the page.</div>","<p><b>진동 알림 (VIB)</b><br>홈 메뉴에서 <span class='kb'>VIB: ON/OFF</span>를 선택하고 PLAY를 누르면 즉시 토글됩니다.</p><p>VIB ON일 때 진동이 오는 타이밍:</p><table class='st'><tr><th style='text-align:left'>타이밍</th><th>설명</th></tr><tr><td style='text-align:left'>수신기 작동 시작 시</td><td>대기 시간이 끝나고 수신기가 실제로 작동하기 시작할 때 진동 1회</td></tr><tr><td style='text-align:left'>수신기 작동 완료 직전</td><td>작동이 끝나기 약 0.5초 전 진동 1회 (완료 예고)</td></tr></table><div class='wb'>⚠ HOLD 모드에서는 진동이 작동하지 않습니다.</div><p style='margin-top:14px;'><b>화면 언어 바꾸기</b><br>홈 메뉴 → <span class='kb'>언어</span> → PLAY. 위·아래 버튼으로 원하는 언어를 고른 뒤 PLAY로 즉시 적용됩니다. 현재 선택된 언어는 <b>[이름]</b> 형식으로 표시됩니다.</p><div class='tb'>💡 이 웹 페이지의 언어는 화면 상단의 언어 버튼(EN / 한국어 / 中文 등)으로 바꿀 수 있습니다.</div>","<p><b>振动提醒 (VIB)</b><br>在主菜单选择 <span class='kb'>VIB: ON/OFF</span> 后按 PLAY 立即切换。</p><p>VIB ON时的振动时机：</p><table class='st'><tr><th style='text-align:left'>时机</th><th>说明</th></tr><tr><td style='text-align:left'>接收器开始动作</td><td>等待时间结束、接收器实际开始动作时振动1次</td></tr><tr><td style='text-align:left'>接收器即将完成</td><td>动作结束前约0.5秒振动1次（完成预告）</td></tr></table><div class='wb'>⚠ HOLD模式下不振动。</div><p style='margin-top:14px;'><b>更改显示语言</b><br>主菜单 → <span class='kb'>语言</span> → PLAY。用上/下键选择语言，按PLAY立即应用。当前选择的语言以 <b>[名称]</b> 格式显示。</p><div class='tb'>💡 此网页的语言可通过页面顶部的语言按钮（EN / 한국어 / 中文等）更改。</div>","<p><b>バイブレーション通知 (VIB)</b><br>ホームメニューで <span class='kb'>VIB: ON/OFF</span> を選択しPLAYを押すと即座にトグルします。</p><p>VIB ONのときの振動タイミング：</p><table class='st'><tr><th style='text-align:left'>タイミング</th><th>説明</th></tr><tr><td style='text-align:left'>受信機が動作開始</td><td>待機時間が終わり受信機が実際に動き始めたとき1回振動</td></tr><tr><td style='text-align:left'>受信機の動作完了直前</td><td>動作終了約0.5秒前に1回振動（完了予告）</td></tr></table><div class='wb'>⚠ HOLDモードでは振動しません。</div><p style='margin-top:14px;'><b>表示言語の変更</b><br>ホームメニュー → <span class='kb'>言語</span> → PLAY。上下ボタンで言語を選びPLAYで即座に適用。現在の言語は <b>[名称]</b> 形式で表示されます。</p><div class='tb'>💡 このウェブページの言語はページ上部の言語ボタン（EN / 한국어 / 中文 など）で変更できます。</div>","<p><b>Vibrationsmeldung (VIB)</b><br>Im Hauptmenü <span class='kb'>VIB: EIN/AUS</span> auswählen und PLAY drücken zum sofortigen Umschalten.</p><p>VIB EIN — Vibrationszeitpunkte:</p><table class='st'><tr><th style='text-align:left'>Zeitpunkt</th><th>Beschreibung</th></tr><tr><td style='text-align:left'>Empfänger startet</td><td>Vibriert einmal, wenn die Verzögerung abläuft und der Empfänger tatsächlich startet</td></tr><tr><td style='text-align:left'>Empfänger fast fertig</td><td>Vibriert einmal ~0,5 s vor Ende (Vorankündigung)</td></tr></table><div class='wb'>⚠ Keine Vibration im HOLD-Modus.</div><p style='margin-top:14px;'><b>Anzeigesprache ändern</b><br>Hauptmenü → <span class='kb'>Sprache</span> → PLAY. Mit UP/DOWN Sprache wählen, dann PLAY zum sofortigen Anwenden. Aktuelle Sprache in <b>[Klammern]</b>.</p><div class='tb'>💡 Die Sprache dieser Webseite kann über den Sprachknopf (EN / 한국어 / 中文 usw.) oben auf der Seite geändert werden.</div>","<p><b>Alerta de vibración (VIB)</b><br>Selecciona <span class='kb'>VIB: ON/OFF</span> en el menú principal y pulsa PLAY para alternar al instante.</p><p>Momentos de vibración con VIB ON:</p><table class='st'><tr><th style='text-align:left'>Momento</th><th>Descripción</th></tr><tr><td style='text-align:left'>Receptor inicia</td><td>Vibra una vez cuando termina el retardo y el receptor empieza a funcionar</td></tr><tr><td style='text-align:left'>Receptor casi termina</td><td>Vibra una vez ~0,5 s antes del final (aviso previo)</td></tr></table><div class='wb'>⚠ Sin vibración en modo HOLD.</div><p style='margin-top:14px;'><b>Cambiar idioma de pantalla</b><br>Menú principal → <span class='kb'>Idioma</span> → PLAY. Usa UP/DOWN para elegir idioma, luego PLAY para aplicar al instante. El idioma seleccionado se muestra en <b>[corchetes]</b>.</p><div class='tb'>💡 El idioma de esta página web se puede cambiar con el botón de idioma (EN / 한국어 / 中文 etc.) en la parte superior.</div>","<p><b>Alerte vibration (VIB)</b><br>Sélectionnez <span class='kb'>VIB: ON/OFF</span> dans le menu principal et appuyez sur PLAY pour basculer instantanément.</p><p>Moments de vibration avec VIB ON :</p><table class='st'><tr><th style='text-align:left'>Moment</th><th>Description</th></tr><tr><td style='text-align:left'>Démarrage récepteur</td><td>Vibre une fois quand le délai se termine et que le récepteur commence à fonctionner</td></tr><tr><td style='text-align:left'>Récepteur presque fini</td><td>Vibre une fois ~0,5 s avant la fin (avertissement préalable)</td></tr></table><div class='wb'>⚠ Pas de vibration en mode HOLD.</div><p style='margin-top:14px;'><b>Changer la langue d'affichage</b><br>Menu principal → <span class='kb'>Langue</span> → PLAY. UP/DOWN pour choisir la langue, puis PLAY pour appliquer immédiatement. La langue sélectionnée s'affiche en <b>[crochets]</b>.</p><div class='tb'>💡 La langue de cette page web peut être changée avec le bouton de langue (EN / 한국어 / 中文 etc.) en haut de la page.</div>"],

m_s10_body:["<p>After firing, the display shows two phases.</p><p style='font-weight:600;'>① Communication phase (brief, within a few seconds)</p><p>Shows the moment of signal transmission to the receiver.</p><table class='st'><tr><th>Display</th><th>Meaning</th></tr><tr><td><code>Sending</code></td><td>Signal being sent to receiver</td></tr><tr><td><code>OK</code></td><td>Receiver acknowledgement confirmed</td></tr><tr><td><code>FAIL</code></td><td>No response after 5 retries</td></tr></table><p style='font-weight:600;margin-top:12px;'>② Timer monitor (after communication)</p><p>Each receiver's delay and run progress is shown one line each.</p><div class='op'>01: D-45   02: P-03\n03: END    04: ERR\n05: ...</div><table class='st'><tr><th>Display</th><th>Meaning</th></tr><tr><td><code>01: D-45</code></td><td>Device 1 — fires in 45 s (waiting)</td></tr><tr><td><code>02: P-03</code></td><td>Device 2 — running, 3 s remaining</td></tr><tr><td><code>03: END</code></td><td>Device 3 — completed!</td></tr><tr><td><code>04: ERR</code></td><td>Device 4 — no response (check power/distance)</td></tr><tr><td><code>05: ...</code></td><td>Device 5 — sending signal</td></tr></table><div class='tb'>💡 If there are too many receivers to fit on screen, use UP/DOWN to scroll the list.</div>","<p>발사 후 화면은 두 단계로 진행됩니다.</p><p style='font-weight:600;'>① 통신 단계 (잠깐, 수초 이내)</p><p>신호를 수신기에 전송하는 순간을 표시합니다.</p><table class='st'><tr><th>표시</th><th>무슨 뜻인가요?</th></tr><tr><td><code>전송 중</code></td><td>수신기에 신호 보내는 중</td></tr><tr><td><code>OK</code></td><td>수신기 응답 확인 완료</td></tr><tr><td><code>FAIL</code></td><td>5회 재시도 후에도 응답 없음</td></tr></table><p style='font-weight:600;margin-top:12px;'>② 타이머 모니터 (통신 완료 후)</p><p>각 수신기의 대기·작동 진행 상황이 한 줄씩 표시됩니다.</p><div class='op'>01: D-45   02: P-03\n03: END    04: ERR\n05: ...</div><table class='st'><tr><th>표시</th><th>무슨 뜻인가요?</th></tr><tr><td><code>01: D-45</code></td><td>1번 기기 — 45초 뒤에 작동 예정 (대기 중)</td></tr><tr><td><code>02: P-03</code></td><td>2번 기기 — 지금 작동 중, 3초 남음</td></tr><tr><td><code>03: END</code></td><td>3번 기기 — 작동 완료!</td></tr><tr><td><code>04: ERR</code></td><td>4번 기기 — 응답 없음 (전원·거리 확인)</td></tr><tr><td><code>05: ...</code></td><td>5번 기기 — 신호 보내는 중</td></tr></table><div class='tb'>💡 수신기가 많아서 화면에 다 안 보이면 UP·DOWN으로 목록을 위아래로 스크롤할 수 있습니다.</div>","<p>发射后，画面分两个阶段显示。</p><p style='font-weight:600;'>① 通信阶段（短暂，数秒内）</p><p>显示信号发送到接收器的瞬间。</p><table class='st'><tr><th>显示</th><th>含义</th></tr><tr><td><code>发送中</code></td><td>正在向接收器发送信号</td></tr><tr><td><code>OK</code></td><td>已确认接收器响应</td></tr><tr><td><code>FAIL</code></td><td>5次重试后仍无响应</td></tr></table><p style='font-weight:600;margin-top:12px;'>② 计时器监视（通信完成后）</p><p>每个接收器的等待/运行进度逐行显示。</p><div class='op'>01: D-45   02: P-03\n03: END    04: ERR\n05: ...</div><table class='st'><tr><th>显示</th><th>含义</th></tr><tr><td><code>01: D-45</code></td><td>1号设备 — 45秒后动作（等待中）</td></tr><tr><td><code>02: P-03</code></td><td>2号设备 — 正在运行，剩余3秒</td></tr><tr><td><code>03: END</code></td><td>3号设备 — 动作完成！</td></tr><tr><td><code>04: ERR</code></td><td>4号设备 — 无响应（检查电源/距离）</td></tr><tr><td><code>05: ...</code></td><td>5号设备 — 正在发送信号</td></tr></table><div class='tb'>💡 若接收器太多无法全部显示，可用上/下键上下滚动列表。</div>","<p>発射後、画面は2つのフェーズで進みます。</p><p style='font-weight:600;'>① 通信フェーズ（短時間、数秒以内）</p><p>受信機への信号送信の瞬間を表示します。</p><table class='st'><tr><th>表示</th><th>意味</th></tr><tr><td><code>送信中</code></td><td>受信機に信号送信中</td></tr><tr><td><code>OK</code></td><td>受信機の応答確認完了</td></tr><tr><td><code>FAIL</code></td><td>5回再試行後も応答なし</td></tr></table><p style='font-weight:600;margin-top:12px;'>② タイマーモニター（通信完了後）</p><p>各受信機の待機・動作の進行状況が1行ずつ表示されます。</p><div class='op'>01: D-45   02: P-03\n03: END    04: ERR\n05: ...</div><table class='st'><tr><th>表示</th><th>意味</th></tr><tr><td><code>01: D-45</code></td><td>1番機器 — 45秒後に動作予定（待機中）</td></tr><tr><td><code>02: P-03</code></td><td>2番機器 — 動作中、残り3秒</td></tr><tr><td><code>03: END</code></td><td>3番機器 — 動作完了！</td></tr><tr><td><code>04: ERR</code></td><td>4番機器 — 応答なし（電源・距離確認）</td></tr><tr><td><code>05: ...</code></td><td>5番機器 — 信号送信中</td></tr></table><div class='tb'>💡 受信機が多くて画面に収まらない場合は、上下ボタンでリストをスクロールできます。</div>","<p>Nach dem Auslösen zeigt der Bildschirm zwei Phasen.</p><p style='font-weight:600;'>① Kommunikationsphase (kurz, innerhalb weniger Sekunden)</p><p>Zeigt den Moment der Signalübertragung zum Empfänger.</p><table class='st'><tr><th>Anzeige</th><th>Bedeutung</th></tr><tr><td><code>Sende...</code></td><td>Signal wird an Empfänger gesendet</td></tr><tr><td><code>OK</code></td><td>Empfängerbestätigung erhalten</td></tr><tr><td><code>FAIL</code></td><td>Keine Antwort nach 5 Versuchen</td></tr></table><p style='font-weight:600;margin-top:12px;'>② Timer-Monitor (nach Kommunikation)</p><p>Verzögerungs- und Betriebsfortschritt jedes Empfängers wird zeilenweise angezeigt.</p><div class='op'>01: D-45   02: P-03\n03: END    04: ERR\n05: ...</div><table class='st'><tr><th>Anzeige</th><th>Bedeutung</th></tr><tr><td><code>01: D-45</code></td><td>Gerät 1 — startet in 45 s (wartend)</td></tr><tr><td><code>02: P-03</code></td><td>Gerät 2 — läuft, noch 3 s</td></tr><tr><td><code>03: END</code></td><td>Gerät 3 — abgeschlossen!</td></tr><tr><td><code>04: ERR</code></td><td>Gerät 4 — keine Antwort (Strom/Abstand prüfen)</td></tr><tr><td><code>05: ...</code></td><td>Gerät 5 — Signal wird gesendet</td></tr></table><div class='tb'>💡 Wenn zu viele Empfänger für den Bildschirm sind, scrollen Sie mit UP/DOWN durch die Liste.</div>","<p>Tras disparar, la pantalla muestra dos fases.</p><p style='font-weight:600;'>① Fase de comunicación (breve, en pocos segundos)</p><p>Muestra el momento de transmisión de señal al receptor.</p><table class='st'><tr><th>Indicador</th><th>Significado</th></tr><tr><td><code>Enviando</code></td><td>Señal siendo enviada al receptor</td></tr><tr><td><code>OK</code></td><td>Confirmación del receptor recibida</td></tr><tr><td><code>FAIL</code></td><td>Sin respuesta tras 5 intentos</td></tr></table><p style='font-weight:600;margin-top:12px;'>② Monitor de temporizador (tras la comunicación)</p><p>El progreso de espera y ejecución de cada receptor se muestra línea por línea.</p><div class='op'>01: D-45   02: P-03\n03: END    04: ERR\n05: ...</div><table class='st'><tr><th>Indicador</th><th>Significado</th></tr><tr><td><code>01: D-45</code></td><td>Dispositivo 1 — dispara en 45 s (esperando)</td></tr><tr><td><code>02: P-03</code></td><td>Dispositivo 2 — en marcha, 3 s restantes</td></tr><tr><td><code>03: END</code></td><td>Dispositivo 3 — ¡completado!</td></tr><tr><td><code>04: ERR</code></td><td>Dispositivo 4 — sin respuesta (verificar alimentación/distancia)</td></tr><tr><td><code>05: ...</code></td><td>Dispositivo 5 — enviando señal</td></tr></table><div class='tb'>💡 Si hay demasiados receptores para la pantalla, usa UP/DOWN para desplazarte por la lista.</div>","<p>Après le déclenchement, l'écran affiche deux phases.</p><p style='font-weight:600;'>① Phase de communication (brève, en quelques secondes)</p><p>Affiche le moment de transmission du signal au récepteur.</p><table class='st'><tr><th>Affichage</th><th>Signification</th></tr><tr><td><code>Envoi...</code></td><td>Signal envoyé au récepteur</td></tr><tr><td><code>OK</code></td><td>Confirmation du récepteur reçue</td></tr><tr><td><code>FAIL</code></td><td>Pas de réponse après 5 essais</td></tr></table><p style='font-weight:600;margin-top:12px;'>② Moniteur de minuterie (après la communication)</p><p>L'avancement du délai et de l'exécution de chaque récepteur est affiché ligne par ligne.</p><div class='op'>01: D-45   02: P-03\n03: END    04: ERR\n05: ...</div><table class='st'><tr><th>Affichage</th><th>Signification</th></tr><tr><td><code>01: D-45</code></td><td>Appareil 1 — déclenche dans 45 s (en attente)</td></tr><tr><td><code>02: P-03</code></td><td>Appareil 2 — en cours, 3 s restantes</td></tr><tr><td><code>03: END</code></td><td>Appareil 3 — terminé !</td></tr><tr><td><code>04: ERR</code></td><td>Appareil 4 — pas de réponse (vérifier alimentation/distance)</td></tr><tr><td><code>05: ...</code></td><td>Appareil 5 — signal en cours d'envoi</td></tr></table><div class='tb'>💡 Si trop de récepteurs pour l'écran, utilisez UP/DOWN pour faire défiler la liste.</div>"],

m_s11_body:["<p>Performance examples and output control characteristics for the 4 receiver types supported by the transmitter.</p><table class='st'><tr><th>Type</th><th>Output Control</th><th>Recommended use cases</th></tr><tr><td><b>POT</b></td><td>PWM (0–100%)</td><td>- <b>Solenoid latch release</b>: unlock boxes, hidden doors, drawers<br>- <b>Servo/motor control</b>: blooming flowers, coin-drop gimmicks<br>- Adjust output to control driving speed or torque</td></tr><tr><td><b>SMOKE</b></td><td>Digital ON/OFF</td><td>- <b>Heating coil</b>: hand smoke, shoe smoke, cup smoke, burning card effects<br>- Even at partial power (e.g. 50%), the coil operates at full ON/OFF. Set <b>POWER: 100%</b>.<br>- ⚠️ <b>Caution</b>: Set PLAY time to the minimum needed (usually 1–3 s) to prevent overheating</td></tr><tr><td><b>FOUNTAIN</b></td><td>PWM (0–100%)</td><td>- <b>Electronic ignition</b>: flash paper/cotton instant ignition, touch-ignition gimmicks<br>- <b>Fountain pyro</b>: stage electronic pyrotechnic fountains, firework launches<br>- Internal high-voltage boost circuit ensures reliable ignition with a powerful current burst</td></tr><tr><td><b>MAGNET</b></td><td>Digital ON/OFF</td><td>- <b>Electromagnet lock</b>: magnetic card case lock/release, secret box locks<br>- Internal boost circuit raises voltage to maintain strong attraction or ensure reliable release<br>- 100% digital output — set <b>POWER: 100%</b> in transmitter settings</td></tr></table>","<p>송신기에서 지원하는 4가지 수신기 기기 타입별 연출 예시 및 출력 제어 특징입니다.</p><table class='st'><tr><th>수신기 타입</th><th>출력 제어 방식</th><th>권장 마술 연출 및 응용 예시</th></tr><tr><td><b>POT</b></td><td>PWM (0%~100%)</td><td>- <b>솔레노이드 래치 개방</b>: 상자, 비밀문, 서랍 장치 잠금 해제<br>- <b>서보/모터 제어</b>: 조화 피우기, 동전 떨어뜨리기 기믹<br>- 출력 세기를 조절하여 구동 속도나 토크 제어가 가능합니다</td></tr><tr><td><b>SMOKE</b></td><td>디지털 ON/OFF</td><td>- <b>연기 코일 가열</b>: 손안의 연기, 신발 연기, 컵 내부 연기, 카드 불타는 연기 연출<br>- 중간 출력 세기(예: 50%)를 주어도 물리적으로는 100% 켜짐/꺼짐으로만 작동합니다. 따라서 <b>POWER: 100%</b>로 설정하고 사용하십시오.<br>- ⚠️ <b>주의</b>: 가열 코일이 과열되어 단선되거나 기믹이 손상되는 것을 방지하기 위해 재생 시간(PLAY)은 필요한 최소 시간(보통 1초~3초)으로 설정하세요</td></tr><tr><td><b>FOUNTAIN</b></td><td>PWM (0%~100%)</td><td>- <b>전자 점화</b>: 플래시 페이퍼/코튼 순간 점화, 터치형 점화 기믹<br>- <b>분수 불꽃</b>: 무대 연출용 전자 pyro 분수 불꽃 점화, 폭죽 발사<br>- 내부 고전압 부스트 회로가 내장되어 있어 순간적으로 강력한 전류를 흘려 확실한 점화를 보장합니다</td></tr><tr><td><b>MAGNET</b></td><td>디지털 ON/OFF</td><td>- <b>전자석 잠금</b>: 마그네틱 카드 케이스 잠금/해제, 비밀 함 잠금 장치<br>- 내부 부스트 회로가 순간 전압을 높여 전자석의 흡착력을 강하게 유지 또는 확실히 해제되도록 제어합니다.<br>- 100% 디지털 출력이므로 송신기 설정에서 <b>POWER: 100%</b> 설정을 유지해 주십시오</td></tr></table>","<p>发射器支持的4种接收器类型的演示示例及输出控制特征。</p><table class='st'><tr><th>接收器类型</th><th>输出控制方式</th><th>推荐魔术演示及应用示例</th></tr><tr><td><b>POT</b></td><td>PWM (0%~100%)</td><td>- <b>螺线管锁定开启</b>：解锁箱子、暗门、抽屉装置<br>- <b>舵机/电机控制</b>：花朵盛开、硬币掉落机关<br>- 可通过调节输出强度来控制驱动速度或扭矩</td></tr><tr><td><b>SMOKE</b></td><td>数字 ON/OFF</td><td>- <b>加热烟雾线圈</b>：手中烟雾、鞋内烟雾、杯内烟雾、卡片燃烧烟雾效果<br>- 即使给予中等输出（如50%），物理上仍以100% ON/OFF动作。请将<b>POWER设置为100%</b>。<br>- ⚠️ <b>注意</b>：请将播放时间(PLAY)设置为所需最短时间（通常1~3秒），防止加热线圈过热断路或机关损坏</td></tr><tr><td><b>FOUNTAIN</b></td><td>PWM (0%~100%)</td><td>- <b>电子点火</b>：闪光纸/棉花瞬间点火、触发式点火机关<br>- <b>喷泉焰火</b>：舞台用电子焰火喷泉点火、烟花发射<br>- 内置高压升压电路，瞬间输出强电流确保可靠点火</td></tr><tr><td><b>MAGNET</b></td><td>数字 ON/OFF</td><td>- <b>电磁锁</b>：磁性卡盒锁定/解锁、秘密盒锁定装置<br>- 内置升压电路瞬间提高电压，确保电磁铁强力吸附或可靠释放<br>- 100%数字输出，请在发射器设置中保持<b>POWER: 100%</b></td></tr></table>","<p>送信機がサポートする4種類の受信機タイプ別の演出例と出力制御特性です。</p><table class='st'><tr><th>受信機タイプ</th><th>出力制御方式</th><th>推奨マジック演出・応用例</th></tr><tr><td><b>POT</b></td><td>PWM (0%~100%)</td><td>- <b>ソレノイドラッチ開放</b>：箱・隠し扉・引き出し装置のロック解除<br>- <b>サーボ/モーター制御</b>：花開き・コイン落下ギミック<br>- 出力強度を調整して駆動速度やトルクを制御できます</td></tr><tr><td><b>SMOKE</b></td><td>デジタル ON/OFF</td><td>- <b>煙コイル加熱</b>：手の煙・靴の煙・カップ内部の煙・カード燃焼煙演出<br>- 中間出力（例：50%）を与えても物理的には100% ON/OFFのみ動作します。<b>POWER: 100%</b>に設定してください。<br>- ⚠️ <b>注意</b>：コイル過熱による断線やギミック損傷防止のため、再生時間(PLAY)は必要最小限（通常1〜3秒）に設定してください</td></tr><tr><td><b>FOUNTAIN</b></td><td>PWM (0%~100%)</td><td>- <b>電子点火</b>：フラッシュペーパー/コットン瞬間点火・タッチ式点火ギミック<br>- <b>噴水花火</b>：舞台用電子pyro噴水花火点火・花火発射<br>- 内蔵高電圧ブースト回路が瞬間的に強力な電流を流し確実な点火を保証します</td></tr><tr><td><b>MAGNET</b></td><td>デジタル ON/OFF</td><td>- <b>電磁石ロック</b>：マグネットカードケースのロック/解除・秘密箱ロック装置<br>- 内蔵ブースト回路が瞬間電圧を上げ、電磁石の吸着力を強く維持または確実に解除するよう制御します。<br>- 100%デジタル出力のため送信機設定で<b>POWER: 100%</b>を維持してください</td></tr></table>","<p>Vorführungsbeispiele und Ausgangssteuerungsmerkmale für die 4 unterstützten Empfängertypen.</p><table class='st'><tr><th>Typ</th><th>Ausgangssteuerung</th><th>Empfohlene Anwendungsfälle</th></tr><tr><td><b>POT</b></td><td>PWM (0–100%)</td><td>- <b>Solenoid-Riegel öffnen</b>: Schlösser von Boxen, Geheimtüren, Schubladen entsperren<br>- <b>Servo-/Motorsteuerung</b>: Blühen lassen, Münze fallen lassen<br>- Ausgangsstärke anpassen für Antriebsgeschwindigkeit oder Drehmoment</td></tr><tr><td><b>SMOKE</b></td><td>Digital EIN/AUS</td><td>- <b>Heizspule</b>: Rauch in der Hand, im Schuh, in der Tasse, brennende Karte<br>- Auch bei Teilleistung (z. B. 50%) arbeitet die Spule nur EIN/AUS. Verwenden Sie <b>POWER: 100%</b>.<br>- ⚠️ <b>Achtung</b>: PLAY-Zeit auf das nötige Minimum setzen (normalerweise 1–3 s) um Überhitzung zu vermeiden</td></tr><tr><td><b>FOUNTAIN</b></td><td>PWM (0–100%)</td><td>- <b>Elektronische Zündung</b>: Flash-Papier/Baumwolle Sofortzündung, Touch-Zündungseffekte<br>- <b>Pyro-Brunnen</b>: Bühnenpyrotechnik, Feuerwerk-Abschuss<br>- Integrierter Hochspannungsbooster liefert starken Stromimpuls für sichere Zündung</td></tr><tr><td><b>MAGNET</b></td><td>Digital EIN/AUS</td><td>- <b>Elektromagnetschloss</b>: Magnetkartengehäuse sperren/entsperren, Geheimboxschlösser<br>- Interner Booster erhöht Spannung für starke Haltekraft oder zuverlässige Freigabe<br>- 100% digitaler Ausgang — <b>POWER: 100%</b> in den Einstellungen behalten</td></tr></table>","<p>Ejemplos de actuación y características de control de salida para los 4 tipos de receptores compatibles.</p><table class='st'><tr><th>Tipo</th><th>Control de salida</th><th>Casos de uso recomendados</th></tr><tr><td><b>POT</b></td><td>PWM (0–100%)</td><td>- <b>Apertura de cierre de solenoide</b>: desbloquear cajas, puertas secretas, cajones<br>- <b>Control de servo/motor</b>: flores que se abren, truco de caída de monedas<br>- Ajusta la potencia para controlar velocidad o par motor</td></tr><tr><td><b>SMOKE</b></td><td>Digital ON/OFF</td><td>- <b>Bobina calefactora</b>: humo en la mano, zapato, taza, carta ardiendo<br>- Incluso con potencia parcial (p. ej. 50%), la bobina solo opera ON/OFF. Usa <b>POWER: 100%</b>.<br>- ⚠️ <b>Precaución</b>: establece el tiempo PLAY al mínimo necesario (normalmente 1–3 s) para evitar sobrecalentamiento</td></tr><tr><td><b>FOUNTAIN</b></td><td>PWM (0–100%)</td><td>- <b>Ignición electrónica</b>: papel flash/algodón ignición instantánea, trucos de encendido por contacto<br>- <b>Fuente pirotécnica</b>: fuentes pyro electrónicas para escenario, lanzamiento de fuegos artificiales<br>- Circuito boost de alto voltaje integrado garantiza ignición fiable con un impulso de corriente potente</td></tr><tr><td><b>MAGNET</b></td><td>Digital ON/OFF</td><td>- <b>Cierre electromagnético</b>: bloqueo/desbloqueo de carcasa de tarjeta magnética, cierres de cajas secretas<br>- Circuito boost eleva el voltaje para mantener fuerte atracción o asegurar liberación fiable<br>- Salida 100% digital — mantener <b>POWER: 100%</b> en los ajustes del transmisor</td></tr></table>","<p>Exemples de performance et caractéristiques de contrôle de sortie pour les 4 types de récepteurs pris en charge.</p><table class='st'><tr><th>Type</th><th>Contrôle sortie</th><th>Cas d'utilisation recommandés</th></tr><tr><td><b>POT</b></td><td>PWM (0–100%)</td><td>- <b>Ouverture de verrou solénoïde</b>: déverrouiller boîtes, portes secrètes, tiroirs<br>- <b>Contrôle servo/moteur</b>: fleurs qui s'ouvrent, tours de pièce qui tombe<br>- Ajustez la puissance pour contrôler la vitesse ou le couple</td></tr><tr><td><b>SMOKE</b></td><td>Numérique ON/OFF</td><td>- <b>Bobine chauffante</b>: fumée dans la main, chaussure, tasse, carte brûlante<br>- Même à puissance partielle (ex. 50%), la bobine fonctionne uniquement ON/OFF. Utilisez <b>POWER: 100%</b>.<br>- ⚠️ <b>Attention</b>: réglez le temps PLAY au minimum nécessaire (généralement 1–3 s) pour éviter la surchauffe</td></tr><tr><td><b>FOUNTAIN</b></td><td>PWM (0–100%)</td><td>- <b>Allumage électronique</b>: papier flash/coton allumage instantané, trucs d'allumage au toucher<br>- <b>Fontaine pyro</b>: fontaines pyrotechniques électroniques de scène, lancement de feux d'artifice<br>- Circuit boost haute tension intégré garantit un allumage fiable avec une impulsion de courant puissante</td></tr><tr><td><b>MAGNET</b></td><td>Numérique ON/OFF</td><td>- <b>Verrou électromagnétique</b>: verrouillage/déverrouillage d'étuis à carte magnétique, serrures de boîtes secrètes<br>- Circuit boost interne élève la tension pour maintenir une forte attraction ou assurer une libération fiable<br>- Sortie 100% numérique — maintenir <b>POWER: 100%</b> dans les paramètres du transmetteur</td></tr></table>"],

m_s3_body:["<p>Select and operate a single receiver. 8 rows are shown; move the cursor (▶) up/down and press PLAY to edit or execute each item.</p><table class='st'><tr><th>Row</th><th>Item</th><th>Description</th></tr><tr><td>1</td><td>PLAY</td><td>Press PLAY on this row to fire. TIMER: one press executes. HOLD: output only while held</td></tr><tr><td>2</td><td>ID</td><td>Target receiver number (1–20). PLAY to enter edit → UP/DOWN to change → PLAY to save</td></tr><tr><td>3</td><td>STYLE</td><td>Press PLAY to toggle TIMER ↔ HOLD</td></tr><tr><td>4</td><td>STEP</td><td>Current step / total steps. PLAY ×1: select step. PLAY ×2: adjust step count (1–10)</td></tr><tr><td>5</td><td>TYPE</td><td>Device type filter (POT/SMOKE/FOUNTAIN/REEL/MAGNET/ALL). PLAY to edit → UP/DOWN → PLAY to save</td></tr><tr><td>6</td><td>DELAY</td><td>Delay time for this step. PLAY to enter edit screen</td></tr><tr><td>7</td><td>PLAY(time)</td><td>Run time for this step. PLAY to enter edit screen</td></tr><tr><td>8</td><td>POWER</td><td>Output power (0–100%). PLAY to enter edit screen. (Note: SMOKE and MAGNET types use digital ON/OFF; always keep at 100%)</td></tr></table><div class='op'>▶ PLAY\nID   : 03\nSTYLE: TIMER\nSTEP : 1/2\nTYPE : POT\nDELAY: 00m05s\nPLAY : 02s\nPOWER: 80%</div><p style='margin-top:10px;font-weight:600;'>After firing — display meanings:</p><table class='st'><tr><th>Display</th><th>Meaning</th></tr><tr><td><code>D-45</code></td><td>Waiting — fires in 45 s</td></tr><tr><td><code>P-03</code></td><td>Running — 3 s remaining</td></tr><tr><td><code>END</code></td><td>Completed!</td></tr><tr><td><code>...</code></td><td>Sending signal to receiver</td></tr><tr><td><code>ERR</code></td><td>No response after 5 retries</td></tr></table><div class='tb'>💡 Multiple STEPs run automatically: delay→run→delay→run in sequence. Up to 10 steps.</div><div class='wb'>⚠ If ERR appears, check the receiver is powered on and within range. Try Auto Channel again.</div>","<p>원하는 수신기 한 대만 골라서 작동시킵니다. 화면에는 8개 행이 표시되며, 커서(▶)를 위아래로 이동한 뒤 PLAY로 각 항목을 편집하거나 실행합니다.</p><table class='st'><tr><th>행</th><th>항목</th><th>설명</th></tr><tr><td>1</td><td>PLAY</td><td>이 행에서 PLAY를 누르면 발사. TIMER 모드: 한 번 누르면 실행. HOLD 모드: 누르고 있는 동안만 출력</td></tr><tr><td>2</td><td>ID</td><td>대상 수신기 번호(1~20). PLAY로 편집 진입 → UP/DOWN으로 변경 → PLAY로 저장</td></tr><tr><td>3</td><td>STYLE</td><td>PLAY를 누르면 TIMER ↔ HOLD 전환</td></tr><tr><td>4</td><td>STEP</td><td>현재 스텝/전체 스텝 수. PLAY 1회: 스텝 선택, PLAY 2회: 스텝 수 조정(1~10)</td></tr><tr><td>5</td><td>TYPE</td><td>기기 종류 필터(POT/SMOKE/FOUNTAIN/REEL/MAGNET/ALL). PLAY로 편집 진입 → UP/DOWN으로 변경 → PLAY로 저장</td></tr><tr><td>6</td><td>DELAY</td><td>현재 스텝의 대기 시간. PLAY로 편집 화면 진입</td></tr><tr><td>7</td><td>PLAY(시간)</td><td>현재 스텝의 작동 시간. PLAY로 편집 화면 진입</td></tr><tr><td>8</td><td>POWER</td><td>출력 세기(0~100%). PLAY로 편집 화면 진입. (주의: SMOKE·MAGNET 타입은 디지털 ON/OFF — 항상 100%로 유지)</td></tr></table><div class='op'>▶ PLAY\nID   : 03\nSTYLE: TIMER\nSTEP : 1/2\nTYPE : POT\nDELAY: 00m05s\nPLAY : 02s\nPOWER: 80%</div><p style='margin-top:10px;font-weight:600;'>발사 후 화면에 보이는 것:</p><table class='st'><tr><th>표시</th><th>무슨 뜻인가요?</th></tr><tr><td><code>D-45</code></td><td>45초 뒤에 작동 예정 (대기 중)</td></tr><tr><td><code>P-03</code></td><td>지금 작동 중 — 3초 남음</td></tr><tr><td><code>END</code></td><td>작동 완료!</td></tr><tr><td><code>...</code></td><td>수신기에 신호 보내는 중</td></tr><tr><td><code>ERR</code></td><td>5회 재시도 후에도 응답 없음</td></tr></table><div class='tb'>💡 STEP을 여러 개 설정하면 대기→작동→대기→작동 순으로 자동 반복 실행됩니다. 스텝 수는 최대 10개.</div><div class='wb'>⚠ ERR이 뜨면 수신기 전원이 켜져 있는지, 너무 멀리 있지는 않은지 확인하세요. 자동 채널을 다시 실행해 보는 것도 좋습니다.</div>","<p>选择单个接收器进行操作。画面显示8行，用光标(▶)上下移动后按PLAY编辑或执行各项。</p><table class='st'><tr><th>行</th><th>项目</th><th>说明</th></tr><tr><td>1</td><td>PLAY</td><td>在此行按PLAY发射。TIMER：单按执行。HOLD：按住期间输出</td></tr><tr><td>2</td><td>ID</td><td>目标接收器编号(1~20)。PLAY进入编辑→上/下键更改→PLAY保存</td></tr><tr><td>3</td><td>STYLE</td><td>按PLAY切换 TIMER ↔ HOLD</td></tr><tr><td>4</td><td>STEP</td><td>当前步骤/总步骤数。PLAY×1：选择步骤，PLAY×2：调整步骤数(1~10)</td></tr><tr><td>5</td><td>TYPE</td><td>设备类型筛选(POT/SMOKE/FOUNTAIN/REEL/MAGNET/ALL)。PLAY进入→上/下更改→PLAY保存</td></tr><tr><td>6</td><td>DELAY</td><td>此步骤的等待时间。按PLAY进入编辑</td></tr><tr><td>7</td><td>PLAY(时间)</td><td>此步骤的运行时间。按PLAY进入编辑</td></tr><tr><td>8</td><td>POWER</td><td>输出功率(0~100%)。按PLAY进入编辑。(注意：SMOKE·MAGNET类型为数字ON/OFF，请保持100%)</td></tr></table><div class='op'>▶ PLAY\nID   : 03\nSTYLE: TIMER\nSTEP : 1/2\nTYPE : POT\nDELAY: 00m05s\nPLAY : 02s\nPOWER: 80%</div><p style='margin-top:10px;font-weight:600;'>发射后屏幕显示：</p><table class='st'><tr><th>显示</th><th>含义</th></tr><tr><td><code>D-45</code></td><td>45秒后动作（等待中）</td></tr><tr><td><code>P-03</code></td><td>正在运行，剩余3秒</td></tr><tr><td><code>END</code></td><td>动作完成！</td></tr><tr><td><code>...</code></td><td>正在向接收器发送信号</td></tr><tr><td><code>ERR</code></td><td>5次重试后仍无响应</td></tr></table><div class='tb'>💡 设置多个STEP可自动循环：等待→运行→等待→运行。最多10个步骤。</div><div class='wb'>⚠ 出现ERR时，请检查接收器电源是否开启、距离是否过远。建议重新运行自动信道。</div>","<p>受信機を1台選んで操作します。8行が表示され、カーソル(▶)を上下に動かしPLAYで編集・実行します。</p><table class='st'><tr><th>行</th><th>項目</th><th>説明</th></tr><tr><td>1</td><td>PLAY</td><td>この行でPLAYを押すと発射。TIMER：1回押して実行。HOLD：押している間だけ出力</td></tr><tr><td>2</td><td>ID</td><td>対象受信機番号(1~20)。PLAYで編集進入→上下で変更→PLAYで保存</td></tr><tr><td>3</td><td>STYLE</td><td>PLAYを押してTIMER↔HOLD切替</td></tr><tr><td>4</td><td>STEP</td><td>現在ステップ/総ステップ数。PLAY×1：ステップ選択、PLAY×2：ステップ数調整(1~10)</td></tr><tr><td>5</td><td>TYPE</td><td>機器タイプフィルタ(POT/SMOKE/FOUNTAIN/REEL/MAGNET/ALL)。PLAYで編集→上下で変更→PLAYで保存</td></tr><tr><td>6</td><td>DELAY</td><td>このステップの待機時間。PLAYで編集画面へ</td></tr><tr><td>7</td><td>PLAY(時間)</td><td>このステップの動作時間。PLAYで編集画面へ</td></tr><tr><td>8</td><td>POWER</td><td>出力強度(0~100%)。PLAYで編集画面へ。(注意：SMOKE・MAGNETタイプはデジタルON/OFF制御、常に100%に設定)</td></tr></table><div class='op'>▶ PLAY\nID   : 03\nSTYLE: TIMER\nSTEP : 1/2\nTYPE : POT\nDELAY: 00m05s\nPLAY : 02s\nPOWER: 80%</div><p style='margin-top:10px;font-weight:600;'>発射後の画面表示：</p><table class='st'><tr><th>表示</th><th>意味</th></tr><tr><td><code>D-45</code></td><td>45秒後に動作予定（待機中）</td></tr><tr><td><code>P-03</code></td><td>動作中 — 残り3秒</td></tr><tr><td><code>END</code></td><td>動作完了！</td></tr><tr><td><code>...</code></td><td>受信機に信号送信中</td></tr><tr><td><code>ERR</code></td><td>5回再試行後も応答なし</td></tr></table><div class='tb'>💡 STEPを複数設定すると待機→動作→待機→動作の順で自動繰り返し実行。最大10ステップ。</div><div class='wb'>⚠ ERRが出たら受信機の電源と距離を確認してください。自動チャネルの再実行も有効です。</div>","<p>Wählen Sie einen Empfänger und bedienen Sie ihn. 8 Zeilen werden angezeigt; Cursor (▶) mit UP/DOWN bewegen, PLAY zum Bearbeiten oder Ausführen.</p><table class='st'><tr><th>Zeile</th><th>Punkt</th><th>Beschreibung</th></tr><tr><td>1</td><td>PLAY</td><td>PLAY in dieser Zeile drücken zum Auslösen. TIMER: einmal drücken. HOLD: nur während Halten</td></tr><tr><td>2</td><td>ID</td><td>Zielnummer des Empfängers (1–20). PLAY zum Bearbeiten → UP/DOWN ändern → PLAY speichern</td></tr><tr><td>3</td><td>STYLE</td><td>PLAY drücken zum Umschalten TIMER ↔ HOLD</td></tr><tr><td>4</td><td>STEP</td><td>Aktueller Schritt/Gesamtschritte. PLAY ×1: Schritt wählen, PLAY ×2: Anzahl anpassen (1–10)</td></tr><tr><td>5</td><td>TYPE</td><td>Gerätetyp-Filter (POT/SMOKE/FOUNTAIN/REEL/MAGNET/ALL). PLAY zum Bearbeiten → UP/DOWN → PLAY speichern</td></tr><tr><td>6</td><td>DELAY</td><td>Verzögerungszeit dieses Schritts. PLAY zum Öffnen</td></tr><tr><td>7</td><td>PLAY(Zeit)</td><td>Laufzeit dieses Schritts. PLAY zum Öffnen</td></tr><tr><td>8</td><td>POWER</td><td>Ausgangsleistung (0–100%). PLAY zum Öffnen. (Hinweis: SMOKE und MAGNET nutzen Digital-EIN/AUS; immer 100% verwenden)</td></tr></table><div class='op'>▶ PLAY\nID   : 03\nSTYLE: TIMER\nSTEP : 1/2\nTYPE : POT\nDELAY: 00m05s\nPLAY : 02s\nPOWER: 80%</div><p style='margin-top:10px;font-weight:600;'>Anzeige nach dem Auslösen:</p><table class='st'><tr><th>Anzeige</th><th>Bedeutung</th></tr><tr><td><code>D-45</code></td><td>Startet in 45 s (wartend)</td></tr><tr><td><code>P-03</code></td><td>Läuft — noch 3 s</td></tr><tr><td><code>END</code></td><td>Abgeschlossen!</td></tr><tr><td><code>...</code></td><td>Signal wird gesendet</td></tr><tr><td><code>ERR</code></td><td>Keine Antwort nach 5 Versuchen</td></tr></table><div class='tb'>💡 Mehrere STEPs laufen automatisch: Warten→Laufen→Warten→Laufen. Bis zu 10 Schritte.</div><div class='wb'>⚠ Bei ERR: Prüfen Sie Stromversorgung und Abstand des Empfängers. Auto-Kanal erneut ausführen.</div>","<p>Selecciona y opera un solo receptor. Se muestran 8 filas; mueve el cursor (▶) con UP/DOWN y pulsa PLAY para editar o ejecutar.</p><table class='st'><tr><th>Fila</th><th>Elemento</th><th>Descripción</th></tr><tr><td>1</td><td>PLAY</td><td>Pulsa PLAY en esta fila para disparar. TIMER: una pulsación ejecuta. HOLD: salida solo mientras se mantiene</td></tr><tr><td>2</td><td>ID</td><td>Número del receptor objetivo (1–20). PLAY para editar → UP/DOWN para cambiar → PLAY para guardar</td></tr><tr><td>3</td><td>STYLE</td><td>Pulsa PLAY para alternar TIMER ↔ HOLD</td></tr><tr><td>4</td><td>STEP</td><td>Paso actual / total pasos. PLAY ×1: seleccionar paso. PLAY ×2: ajustar número (1–10)</td></tr><tr><td>5</td><td>TYPE</td><td>Filtro de tipo de dispositivo (POT/SMOKE/FOUNTAIN/REEL/MAGNET/ALL). PLAY para editar → UP/DOWN → PLAY para guardar</td></tr><tr><td>6</td><td>DELAY</td><td>Tiempo de retardo de este paso. PLAY para entrar al editor</td></tr><tr><td>7</td><td>PLAY(tiempo)</td><td>Tiempo de ejecución de este paso. PLAY para entrar al editor</td></tr><tr><td>8</td><td>POWER</td><td>Potencia de salida (0–100%). PLAY para entrar. (Nota: SMOKE y MAGNET usan ON/OFF digital; mantener siempre al 100%)</td></tr></table><div class='op'>▶ PLAY\nID   : 03\nSTYLE: TIMER\nSTEP : 1/2\nTYPE : POT\nDELAY: 00m05s\nPLAY : 02s\nPOWER: 80%</div><p style='margin-top:10px;font-weight:600;'>Pantalla tras disparar:</p><table class='st'><tr><th>Indicador</th><th>Significado</th></tr><tr><td><code>D-45</code></td><td>Disparo en 45 s (en espera)</td></tr><tr><td><code>P-03</code></td><td>En marcha — 3 s restantes</td></tr><tr><td><code>END</code></td><td>¡Completado!</td></tr><tr><td><code>...</code></td><td>Enviando señal al receptor</td></tr><tr><td><code>ERR</code></td><td>Sin respuesta tras 5 intentos</td></tr></table><div class='tb'>💡 Con varios STEPs se ejecuta automáticamente: espera→marcha→espera→marcha. Hasta 10 pasos.</div><div class='wb'>⚠ Si aparece ERR, comprueba que el receptor esté encendido y dentro del alcance. Ejecuta Auto Canal de nuevo.</div>","<p>Sélectionnez et actionnez un seul récepteur. 8 lignes sont affichées ; déplacez le curseur (▶) avec UP/DOWN et appuyez sur PLAY pour éditer ou exécuter.</p><table class='st'><tr><th>Ligne</th><th>Élément</th><th>Description</th></tr><tr><td>1</td><td>PLAY</td><td>Appuyez sur PLAY sur cette ligne pour déclencher. TIMER : une pression exécute. HOLD : sortie uniquement pendant la pression</td></tr><tr><td>2</td><td>ID</td><td>Numéro du récepteur cible (1–20). PLAY pour éditer → UP/DOWN pour changer → PLAY pour sauvegarder</td></tr><tr><td>3</td><td>STYLE</td><td>Appuyez sur PLAY pour basculer TIMER ↔ HOLD</td></tr><tr><td>4</td><td>STEP</td><td>Étape actuelle / total. PLAY ×1 : sélectionner. PLAY ×2 : ajuster le nombre (1–10)</td></tr><tr><td>5</td><td>TYPE</td><td>Filtre type d'appareil (POT/SMOKE/FOUNTAIN/REEL/MAGNET/ALL). PLAY pour éditer → UP/DOWN → PLAY sauvegarder</td></tr><tr><td>6</td><td>DELAY</td><td>Temps de délai de cette étape. PLAY pour ouvrir l'éditeur</td></tr><tr><td>7</td><td>PLAY(durée)</td><td>Durée d'exécution de cette étape. PLAY pour ouvrir l'éditeur</td></tr><tr><td>8</td><td>POWER</td><td>Puissance de sortie (0–100%). PLAY pour ouvrir. (Note : SMOKE et MAGNET utilisent ON/OFF numérique ; toujours maintenir à 100%)</td></tr></table><div class='op'>▶ PLAY\nID   : 03\nSTYLE: TIMER\nSTEP : 1/2\nTYPE : POT\nDELAY: 00m05s\nPLAY : 02s\nPOWER: 80%</div><p style='margin-top:10px;font-weight:600;'>Affichage après déclenchement :</p><table class='st'><tr><th>Affichage</th><th>Signification</th></tr><tr><td><code>D-45</code></td><td>Déclenche dans 45 s (en attente)</td></tr><tr><td><code>P-03</code></td><td>En cours — 3 s restantes</td></tr><tr><td><code>END</code></td><td>Terminé !</td></tr><tr><td><code>...</code></td><td>Signal envoyé au récepteur</td></tr><tr><td><code>ERR</code></td><td>Pas de réponse après 5 essais</td></tr></table><div class='tb'>💡 Plusieurs STEPs s'exécutent automatiquement : attente→marche→attente→marche. Jusqu'à 10 étapes.</div><div class='wb'>⚠ Si ERR apparaît, vérifiez l'alimentation et la distance du récepteur. Relancez le Canal Auto.</div>"]

};

function wt(k){const a=I18N[k];if(!a)return k;return a[window.LANG_IDX]||a[0]||k;}

function applyI18n(){

    document.querySelectorAll('[data-i18n]').forEach(el=>{const t=wt(el.getAttribute('data-i18n'));if(t)el.textContent=t;});

    document.querySelectorAll('[data-i18n-ph]').forEach(el=>{const t=wt(el.getAttribute('data-i18n-ph'));if(t)el.placeholder=t;});

    document.querySelectorAll('[data-i18n-html]').forEach(function(el){var a=I18N[el.getAttribute('data-i18n-html')];if(a)el.innerHTML=a[window.LANG_IDX]||a[0]||'';});

}

function setLang(idx){localStorage.setItem('nx_lang',idx);fetch('/api/set-language',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'lang='+idx}).then(function(){location.reload();}).catch(function(e){console.error(e);location.reload();});}

function toggleLangMenu(e){e.stopPropagation();document.getElementById('lang-sw').classList.toggle('open');}

document.addEventListener('click',function(e){const sw=document.getElementById('lang-sw');if(sw&&!sw.contains(e.target))sw.classList.remove('open');});

const _LC=['EN','한','中','日','DE','ES','FR'];

function escHtml(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}

function showMessage(text,type='info',duration=3000){let m=document.getElementById('global-message-box');if(!m){m=document.createElement('div');m.id='global-message-box';document.body.appendChild(m);}m.textContent=text;m.className='message-box message-'+type+' show';if(duration>0)setTimeout(()=>m.classList.remove('show'),duration);}

document.addEventListener('DOMContentLoaded',function(){

    applyI18n();

    const cb=document.getElementById('lang-cur-btn');

    if(cb)cb.innerHTML='🌐 '+(_LC[window.LANG_IDX]||'EN')+' ▾';

    document.querySelectorAll('.lang-opt').forEach(b=>b.classList.toggle('active',+b.dataset.lang===window.LANG_IDX));

});

)rawliteral";

void broadcastJson(const JsonDocument& doc) {
    String output;
    serializeJson(doc, output);
    ws.textAll(output);
}

void getWifiStatusJson(JsonDocument& doc) {
    doc["connected"] = (WiFi.status() == WL_CONNECTED);
    if (doc["connected"]) {
        doc["ssid"] = WiFi.SSID();
        doc["rssi"] = WiFi.RSSI();
        doc["ip"] = WiFi.localIP().toString();
    }
    doc["scanning"] = (bool)scan_in_progress;
    doc["connecting"] = (otaWifiStatus == OTA_WIFI_CONNECTING);
}

void broadcastWifiStatus(const char* status, int reason) {
    JsonDocument doc;
    getWifiStatusJson(doc);
    doc["type"] = "wifi_status_update";
    doc["status"] = status;
    if (reason != 0) {
        doc["reason"] = reason;
    }
    broadcastJson(doc);
}

void broadcastOtaStatus() {
    JsonDocument doc;
    bool isConnected = (WiFi.status() == WL_CONNECTED);
    bool checkOk = isConnected && !otaState.latestVersion.isEmpty();

    doc["type"] = "ota_status";
    doc["internet_ok"] = isConnected;
    doc["check_ok"] = checkOk;
    doc["current_version"] = firmwareVersion;
    doc["latest_version"] = isConnected ? otaState.latestVersion : "N/A";
    doc["update_available"] = isConnected ? otaState.updateAvailable : false;
    doc["changelog"] = checkOk ? otaState.changeLog : "";

    broadcastJson(doc);
}

void broadcastOtaProgress(int progress) {
    JsonDocument doc;
    doc["type"] = "ota_progress";
    doc["progress"] = progress;
    broadcastJson(doc);
}

void broadcastOtaResult(bool success, const String& msg) {
    JsonDocument doc;
    doc["type"] = "ota_result";
    doc["success"] = success;
    doc["msg"] = msg;
    broadcastJson(doc);
}


String getPageHeader(const String& title, const char* i18nKey) {

    const char* langCodes[] = {"en","ko","zh","ja","de","es","fr"};

    uint8_t li = (uint8_t)currentLanguage < 7 ? (uint8_t)currentLanguage : 0;

    String html = "<!DOCTYPE html><html lang='";

    html += langCodes[li];

    html += F("'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>");

    html += "<title>" + title + "</title>";

    html += F(R"rawliteral(<style>

@import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap');

body{font-family:'Inter',-apple-system,BlinkMacSystemFont,sans-serif;margin:0;padding:20px 10px;background-color:#0b0b0f;background-image:radial-gradient(circle at 50% 0%,#1e1b4b 0%,#0b0b0f 70%);color:#f3f4f6;text-align:center;min-height:100vh;box-sizing:border-box;}

.container{max-width:500px;margin:40px auto;background:rgba(255,255,255,0.02);padding:30px 24px;border-radius:24px;border:1px solid rgba(255,255,255,0.08);backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);box-shadow:0 20px 50px rgba(0,0,0,0.4);text-align:center;position:relative;}

h1{font-size:26px;font-weight:700;margin-top:0;margin-bottom:10px;background:linear-gradient(135deg,#a78bfa 0%,#8b5cf6 100%);-webkit-background-clip:text;-webkit-text-fill-color:transparent;letter-spacing:-0.5px;}

h2,h3{color:#e5e7eb;font-weight:600;margin-top:0;margin-bottom:12px;font-size:18px;letter-spacing:-0.2px;}

.card{background:rgba(255,255,255,0.015);padding:20px;margin-bottom:20px;border-radius:16px;border:1px solid rgba(255,255,255,0.05);box-shadow:inset 0 1px 1px rgba(255,255,255,0.03);text-align:center;}

.btn{display:inline-flex;align-items:center;justify-content:center;background:linear-gradient(135deg,#6d28d9 0%,#5b21b6 100%);color:#fff;padding:12px 24px;margin:8px 4px;text-decoration:none;border:none;border-radius:12px;cursor:pointer;font-size:15px;font-weight:600;min-width:160px;box-shadow:0 4px 12px rgba(109,40,217,0.2);transition:all 0.25s cubic-bezier(0.4,0,0.2,1);}

.btn:hover{transform:translateY(-2px);box-shadow:0 6px 20px rgba(109,40,217,0.35);background:linear-gradient(135deg,#7c3aed 0%,#6d28d9 100%);}

.btn:active{transform:translateY(0);}

.btn:disabled{background:#4b5563;color:#9ca3af;box-shadow:none;cursor:not-allowed;transform:none;}

.btn-danger{background:linear-gradient(135deg,#dc2626 0%,#b91c1c 100%);box-shadow:0 4px 12px rgba(220,38,38,0.2);}

.btn-danger:hover{background:linear-gradient(135deg,#ef4444 0%,#dc2626 100%);box-shadow:0 6px 20px rgba(220,38,38,0.35);}

.btn-secondary{background:rgba(255,255,255,0.05);color:#e5e7eb;border:1px solid rgba(255,255,255,0.1);box-shadow:none;}

.btn-secondary:hover{background:rgba(255,255,255,0.1);box-shadow:none;}

.btn-success{background:linear-gradient(135deg,#059669 0%,#047857 100%);box-shadow:0 4px 12px rgba(5,150,105,0.2);}

.btn-success:hover{background:linear-gradient(135deg,#10b981 0%,#059669 100%);box-shadow:0 6px 20px rgba(5,150,105,0.35);}

input,select{width:calc(100% - 28px);padding:12px 14px;margin:10px 0;background:rgba(0,0,0,0.3);border:1px solid rgba(255,255,255,0.1);border-radius:12px;color:#fff;font-size:15px;text-align:center;outline:none;transition:all 0.2s ease;}

input:focus,select:focus{border-color:#a78bfa;box-shadow:0 0 0 3px rgba(167,139,250,0.2);background:rgba(0,0,0,0.4);}

.hidden{display:none;}

.form-group{margin-bottom:20px;text-align:left;}

.form-group label{display:block;margin-bottom:6px;font-size:14px;color:#9ca3af;font-weight:500;}

.message-box{padding:14px 20px;border-radius:12px;text-align:center;transition:all 0.3s cubic-bezier(0.4,0,0.2,1);position:fixed;top:20px;left:50%;transform:translate(-50%,-20px);z-index:1000;width:85%;max-width:420px;display:none;opacity:0;box-shadow:0 10px 30px rgba(0,0,0,0.5);font-size:14px;font-weight:500;}

.message-box.show{display:block;opacity:1;transform:translate(-50%,0);}

.message-info{background-color:#1e3a8a;border:1px solid #2563eb;color:#bfdbfe;}

.message-success{background-color:#064e3b;border:1px solid #059669;color:#a7f3d0;}

.message-error{background-color:#7f1d1d;border:1px solid #dc2626;color:#fca5a5;}

.changelog{text-align:left;background:rgba(0,0,0,0.4);padding:15px;border-radius:12px;margin-bottom:15px;border:1px solid rgba(255,255,255,0.05);white-space:pre-wrap;font-size:13px;line-height:1.5;color:#d1d5db;}

.progress-bar{width:90%;max-width:350px;background-color:rgba(255,255,255,0.1);border-radius:8px;overflow:hidden;margin:15px auto;border:1px solid rgba(255,255,255,0.05);}

.progress-bar-inner{height:14px;width:0%;background:linear-gradient(90deg,#10b981,#059669);color:white;text-align:center;line-height:14px;font-size:10px;font-weight:700;transition:width 0.2s ease;}

.notice{font-size:13px;color:#f87171;margin-top:10px;font-weight:500;}

.lang-switcher{position:relative;display:flex;justify-content:flex-end;margin-bottom:8px;z-index:1000;}
.lang-cur{display:flex;align-items:center;gap:5px;background:rgba(167,139,250,0.18);border:1.5px solid rgba(167,139,250,0.5);color:#ddd6fe;border-radius:10px;padding:7px 13px;font-size:13px;font-weight:700;cursor:pointer;font-family:inherit;transition:background 0.2s,border-color 0.2s;white-space:nowrap;letter-spacing:0.02em;}

.lang-cur:hover{background:rgba(167,139,250,0.3);border-color:rgba(167,139,250,0.7);}

.lang-drop{display:none;position:absolute;top:calc(100% + 8px);right:0;background:#18163a;border:1.5px solid rgba(167,139,250,0.4);border-radius:12px;padding:6px;min-width:130px;box-shadow:0 10px 32px rgba(0,0,0,0.7);z-index:100;}

.lang-drop-label{font-size:10px;color:rgba(167,139,250,0.6);text-transform:uppercase;letter-spacing:0.08em;padding:4px 12px 6px;font-weight:600;}

.lang-switcher.open .lang-drop{display:block;}

.lang-opt{display:block;width:100%;text-align:left;background:none;border:none;color:#c4b5fd;padding:8px 14px;font-size:13px;font-weight:500;cursor:pointer;border-radius:8px;font-family:inherit;box-sizing:border-box;}

.lang-opt:hover{background:rgba(167,139,250,0.2);}

.lang-opt.active{color:#e9d5ff;font-weight:700;background:rgba(167,139,250,0.12);}

</style>

<script>

(function(){
    let lang = parseInt(localStorage.getItem('nx_lang'), 10);
    if (isNaN(lang) || lang < 0 || lang > 6) {
        lang = )rawliteral");

    html += String(li);

    html += F(R"rawliteral(;
    }
    window.LANG_IDX = (isNaN(lang) || lang < 0 || lang > 6) ? 0 : lang;
})();
</script><script src='/i18n.js'></script>

</head><body><div class='container'>)rawliteral");

    html += F(R"rawliteral(<div class='lang-switcher' id='lang-sw'><button class='lang-cur' id='lang-cur-btn' onclick='toggleLangMenu(event)'>🌐 EN ▾</button><div class='lang-drop'><div class='lang-drop-label'>Language</div><button class='lang-opt' data-lang='0' onclick='setLang(0)'>EN — English</button><button class='lang-opt' data-lang='1' onclick='setLang(1)'>한국어</button><button class='lang-opt' data-lang='2' onclick='setLang(2)'>中文</button><button class='lang-opt' data-lang='3' onclick='setLang(3)'>日本語</button><button class='lang-opt' data-lang='4' onclick='setLang(4)'>DE — Deutsch</button><button class='lang-opt' data-lang='5' onclick='setLang(5)'>ES — Español</button><button class='lang-opt' data-lang='6' onclick='setLang(6)'>FR — Français</button></div></div>)rawliteral");

    html += F("<h1");

    if (i18nKey) { html += F(" data-i18n='"); html += i18nKey; html += F("'"); }

    html += F(">");

    html += title;

    html += F("</h1>");

    return html;

}



String getPageFooter(bool showHomeButton) {

    String html;

    if (showHomeButton) html += F("<p style='margin-top:25px;'><a href='/' class='btn' data-i18n='back_home'>Back to Home</a></p>");

    html += F("</div></body></html>");

    return html;

}



static void wifiScanTask(void* pvParameters) {

    logPrintf(LogLevel::LOG_INFO, "OTA TASK: Performing Wi-Fi scan asynchronously.");

    int n = WiFi.scanNetworks(false, false);



    // Build results in a local vector first so we hold the mutex for the minimum time.

    std::vector<ScanResult> localResults;

    if (n > 0) {

        localResults.reserve(n);

        for (int i = 0; i < n; ++i) {

            localResults.push_back({WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});

        }

        std::sort(localResults.begin(), localResults.end(), [](const auto &a, const auto &b) { return a.rssi > b.rssi; });

        logPrintf(LogLevel::LOG_INFO, "OTA TASK: Scan found %d networks.", n);

    } else {

        logPrintf(LogLevel::LOG_WARN, "OTA TASK: Scan found no networks.");

    }

    WiFi.scanDelete();



    // Broadcast scan result via websocket
    JsonDocument doc;
    doc["type"] = "scan_result";
    JsonArray networksArray = doc["networks"].to<JsonArray>();
    for (const auto& net : localResults) {
        JsonObject netObj = networksArray.add<JsonObject>();
        netObj["ssid"] = net.ssid;
        netObj["rssi"] = net.rssi;
        netObj["encrypted"] = net.encrypted;
    }
    broadcastJson(doc);

    // Swap under mutex so /api/scan handler never sees a partially-cleared vector.
    if (scanResultsMutex && xSemaphoreTake(scanResultsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        cachedScanResults = std::move(localResults);
        xSemaphoreGive(scanResultsMutex);
    }

    scan_in_progress = false;

    vTaskDelete(NULL);

}



static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {

    switch (event) {

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:

            logPrintf(LogLevel::LOG_INFO, "WiFi Connected! IP: %s", WiFi.localIP().toString().c_str());

            otaWifiStatus = OTA_WIFI_CONNECTED;

            otaConnectingSsid = "";

            // Core 0 → Core 1 안전 지연: EEPROM.commit() / changeMode()를 handleOtaLoop()에서 처리

            strncpy(pendingNetworkSSID, WiFi.SSID().c_str(), 64);

            strncpy(pendingNetworkPWD, wifi_password.c_str(), 64);

            pendingNetworkSave = true;

            // 재연결 시 이미 체크 완료된 경우 중복 실행 방지

            if (!otaVersionChecked) {

                pendingOtaMode = 1; // → MODE_OTA_CHECKING

                otaState.inProgress = true;

            }

            broadcastWifiStatus("connected");

            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {

            int reason = info.wifi_sta_disconnected.reason;

            otaVersionChecked = false;

            otaConnectingSsid = "";

            if (otaWifiStatus == OTA_WIFI_CONNECTING) {

                logPrintf(LogLevel::LOG_WARN, "WiFi Connection Failed. Reason: %d", reason);

                otaWifiStatus = OTA_WIFI_FAILED;

                WiFi.disconnect(false);

                if (isOtaMode(currentMode)) {

                    pendingOtaMode = 2; // → MODE_OTA_WIFI_AP

                }

                broadcastWifiStatus("failed", reason);

            } else if (otaWifiStatus == OTA_WIFI_CONNECTED || otaWifiStatus == OTA_WIFI_DISCONNECTED) {

                logPrintf(LogLevel::LOG_WARN, "WiFi STA Disconnected. Reason: %d", reason);

                otaWifiStatus = OTA_WIFI_IDLE;

                // scan_in_progress 중 채널 전환으로 발생한 일시적 STA 끊김은 무시 (스캔 후 자동 재연결됨)

                if (isOtaMode(currentMode) && !scan_in_progress) {

                    pendingOtaMode = 2; // → MODE_OTA_WIFI_AP

                }

                broadcastWifiStatus("disconnected", reason);

            }

            break;

        }

        default: break;

    }

}



static void startOtaWebApp() {

    logPrintf(LogLevel::LOG_INFO, "OTA: Starting AP+STA mode and web server.");

    currentMode = MODE_OTA_WIFI_AP;

    otaWifiStatus = OTA_WIFI_IDLE;

    otaVersionChecked = false;



    if (!scanResultsMutex) {

        scanResultsMutex = xSemaphoreCreateMutex();

    }



    if (!wifiEventHandlersRegistered) {

        wifi_event_handle = WiFi.onEvent(onWiFiEvent);

        wifiEventHandlersRegistered = true;

    }

    

    WiFi.mode(WIFI_AP_STA);

    WiFi.setAutoReconnect(true);

    WiFi.softAPConfig(OTA_AP_IP, OTA_AP_IP, OTA_AP_SUBNET);

    WiFi.softAP(OTA_AP_PREFIX, NULL, OTA_AP_CHANNEL, 0, OTA_AP_MAX_CONN);

    logPrintf(LogLevel::LOG_INFO, "OTA: SoftAP '%s' started at %s (No Password)", OTA_AP_PREFIX, WiFi.softAPIP().toString().c_str());

    

    if (!webServerInitialized) {

        setupWebServer();

        webServerInitialized = true;

    }

    server.begin();

    updateDisplay();

}



static bool attemptAutoConnect() {

    loadKnownNetworks(); 

    if (knownNetworks.empty()) {

        logPrintf(LogLevel::LOG_INFO, "OTA: No known networks for auto-connect.");

        return false;

    }



    logPrintf(LogLevel::LOG_INFO, "OTA: Found %d known networks. Attempting auto-connect...", knownNetworks.size());



    for (const auto& net : knownNetworks) {

        logPrintf(LogLevel::LOG_INFO, "OTA: Trying to connect to '%s'...", net.ssid.c_str());

        

        otaConnectingSsid = net.ssid;

        currentMode = MODE_OTA_CONNECTING;

        updateDisplay();

        

        wifi_password = net.pass;

        WiFi.begin(net.ssid.c_str(), net.pass.c_str());



        unsigned long startAttemptTime = millis();

        while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_CONNECT_TIMEOUT_MS) {

            vTaskDelay(pdMS_TO_TICKS(100));

            updateDisplay();

        }



        if (WiFi.status() == WL_CONNECTED) {

            logPrintf(LogLevel::LOG_INFO, "OTA: Auto-connect to '%s' successful!", net.ssid.c_str());

            otaConnectingSsid = "";

            return true;

        } else {

            logPrintf(LogLevel::LOG_WARN, "OTA: Failed to connect to '%s'. Trying next...", net.ssid.c_str());

            WiFi.disconnect(false);

            vTaskDelay(pdMS_TO_TICKS(100));

        }

    }



    logPrintf(LogLevel::LOG_WARN, "OTA: Auto-connect failed for all known networks.");

    otaConnectingSsid = "";

    return false;

}



void initOtaWorkflow() {

    logPrintf(LogLevel::LOG_INFO, "OTA: Entering workflow.");

    otaWorkflowActive = true;

    modeHistory.push_back(static_cast<Mode>(currentMode));

    

    deinitEspNow();

    

    currentMode = MODE_OTA_SCANNING;

    updateDisplay();

    vTaskDelay(pdMS_TO_TICKS(100));



    logPrintf(LogLevel::LOG_INFO, "OTA: Performing initial Wi-Fi scan in STA mode...");

    WiFi.mode(WIFI_STA);

    int n = WiFi.scanNetworks(false, false);

    

    cachedScanResults.clear();

    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            cachedScanResults.push_back({WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
        }

        std::sort(cachedScanResults.begin(), cachedScanResults.end(), [](const auto &a, const auto &b) { return a.rssi > b.rssi; });

    }

    WiFi.scanDelete();

    logPrintf(LogLevel::LOG_INFO, "OTA: Initial scan found %d networks.", n);



    WiFi.mode(WIFI_OFF);

    delay(100);

    startOtaWebApp();



    if (!attemptAutoConnect()) {

        logPrintf(LogLevel::LOG_INFO, "OTA: Auto-connect failed. AP is active for manual setup.");

        currentMode = MODE_OTA_WIFI_AP;

        otaConnectingSsid = "";

        updateDisplay();

    }

}



void handleOtaLoop() {

    ws.cleanupClients();

    if (otaRestartRequest) {

        otaRestartRequest = false;

        shutdownWifi();

        logPrintf(LogLevel::LOG_INFO, "OTA: Restarting for firmware update...");

        Serial.end();

        delay(200);

        ESP.restart();

        return;

    }



    // WiFi 이벤트(Core 0)에서 직접 changeMode()/EEPROM.commit()을 호출하는 것은 안전하지 않음.

    // → 플래그만 세우고 Core 1(handleOtaLoop)에서 처리.

    if (pendingNetworkSave) {

        pendingNetworkSave = false;

        saveKnownNetwork(pendingNetworkSSID, pendingNetworkPWD);

    }

    if (pendingOtaMode == 1) { pendingOtaMode = 0; changeMode(MODE_OTA_CHECKING); }

    else if (pendingOtaMode == 2) { pendingOtaMode = 0; changeMode(MODE_OTA_WIFI_AP); }

    else if (pendingOtaMode == 3) { pendingOtaMode = 0; changeMode(MODE_OTA_CONNECTING); }



    // /exit 핸들러가 세운 플래그: 응답 전송 후 Core 1에서 안전하게 WiFi 종료 + ESP-NOW 재초기화

    if (pendingOtaExit) {

        pendingOtaExit = false;

        vTaskDelay(pdMS_TO_TICKS(300)); // async 응답이 전송될 시간 확보

        modeHistory.clear();

        shutdownWifi();

        if (!initEspNow()) changeMode(MODE_ERROR);

        else changeMode(MODE_HOME_MENU);

        return;

    }



    if (otaExitModeRequest != 0) {

        bool toError = (otaExitModeRequest == 1);

        otaExitModeRequest = 0;

        modeHistory.clear();

        changeMode(toError ? MODE_ERROR : MODE_HOME_MENU);

        return;

    }



    if (checkOtaFromApiFlag) {

        checkOtaFromApiFlag = false;

        otaVersionChecked = false; // 웹 UI 수동 재체크 허용

        checkFirmwareVersion();

    }



    if (otaStartDownloadFlag) {

        otaStartDownloadFlag = false;

        currentMode = MODE_OTA_DOWNLOADING;

        updateDisplay();

        otaState.updateConfirmed = true;

    }



    if (otaWifiDisconnectFlag) {

        otaWifiDisconnectFlag = false;

        otaWifiStatus = OTA_WIFI_IDLE; // Reset from DISCONNECTED so auto-reconnect state is clean.

        if (isOtaMode(currentMode)) {

            currentMode = MODE_OTA_WIFI_AP;

            updateDisplay();

        }

    }



    if (currentMode == MODE_OTA_CHECKING && otaState.inProgress) {

        otaState.inProgress = false;

        checkFirmwareVersion();

    }



    if (currentMode == MODE_OTA_DOWNLOADING && otaState.updateConfirmed) {

        otaState.updateConfirmed = false;

        performOtaUpdate();

    }



    if (otaWifiStatus == OTA_WIFI_CONNECTING && (millis() - wifiConnectStartMillis > WIFI_CONNECT_TIMEOUT_MS)) {

        logPrintf(LogLevel::LOG_WARN, "OTA: Wi-Fi connection timed out.");

        otaWifiStatus = OTA_WIFI_FAILED;

        WiFi.disconnect(false);

        if (isOtaMode(currentMode)) {

            changeMode(MODE_OTA_WIFI_AP);

        }

    }

}



void shutdownWifi() {

    ws.closeAll();

    server.end();

    // Keep webServerInitialized true to prevent duplicate route registrations when restarting the server.



    if (wifiEventHandlersRegistered) {

        WiFi.removeEvent(wifi_event_handle);

        wifiEventHandlersRegistered = false;

    }



    WiFi.softAPdisconnect(true);

    WiFi.disconnect(true, true);



    // Keep scanResultsMutex allocated to prevent crashes if wifiScanTask is still running.
    if (scanResultsMutex && xSemaphoreTake(scanResultsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cachedScanResults.clear();
        xSemaphoreGive(scanResultsMutex);
    }



    otaWifiStatus = OTA_WIFI_IDLE;

    otaWorkflowActive = false;

    logPrintf(LogLevel::LOG_INFO, "OTA: Wi-Fi interfaces have been shut down.");

}



static void checkVersionHttpTask(void* pvParameters) {
    // 스코프 블록: vTaskDelete(NULL)은 C++ 스택 언와인딩을 건너뜀.
    // 블록 종료 시 HTTPClient·WiFiClientSecure 소멸자를 정상 호출해 TLS 힙 누수 방지.
    {
        HTTPClient http;
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(15);
        http.setTimeout(15000);
        http.begin(client, OTA_VERSION_URL);
        int code = http.GET();
        versionCheckCode = code;
        if (code == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);
            if (!err) {
                otaState.latestVersion   = doc["version"].as<String>();
                otaState.changeLog       = doc["notes"].as<String>();
                otaState.firmwareUrl     = doc["url"].as<String>();
                otaState.updateAvailable = isVersionNewer(otaState.latestVersion, firmwareVersion);
                versionCheckParsed = true;
            } else {
                logPrintf(LogLevel::LOG_ERROR, "OTA: Version JSON parsing failed: %s", err.c_str());
                versionCheckParsed = false;
            }
        }
        http.end();
        // http, client, doc 소멸자 호출 → TLS 버퍼 반환
    }
    xSemaphoreGive(versionCheckSem);
    vTaskDelete(NULL);
}

void checkFirmwareVersion() {
    if (!versionCheckSem) {
        versionCheckSem = xSemaphoreCreateBinary();
    } else {
        // Clear any stale semaphore token before starting the task
        xSemaphoreTake(versionCheckSem, 0);
    }
    versionCheckCode   = 0;
    versionCheckParsed = false;
    otaErrorMessage    = ""; // Reset error from previous attempt

    // HTTP task runs on Core 0; loopTask keeps resetting the WDT while it waits
    // Reduced stack from 10KB to 8KB to save memory and prevent allocation failures
    BaseType_t res = xTaskCreatePinnedToCore(checkVersionHttpTask, "otaVerChk", 8192, nullptr, 5, nullptr, 0);
    if (res != pdPASS) {
        logPrintf(LogLevel::LOG_ERROR, "OTA: Failed to create checkVersionHttpTask (low memory)!");
        otaErrorMessage = "Memory Error";
        currentMode = MODE_OTA_ERROR;
        updateDisplay();
        broadcastOtaStatus();
        return;
    }

    // Safety timeout of 25 seconds to prevent permanent lockup if DNS or host hangs
    unsigned long startWait = millis();
    bool success = false;
    while (millis() - startWait < 25000) {
        if (xSemaphoreTake(versionCheckSem, pdMS_TO_TICKS(100)) == pdTRUE) {
            success = true;
            break;
        }
    }

    if (!success) {
        logPrintf(LogLevel::LOG_ERROR, "OTA: Version check timed out!");
        otaErrorMessage = "Check Timed Out";
        currentMode = MODE_OTA_ERROR;
        updateDisplay();
        broadcastOtaStatus();
        return;
    }

    int httpCode = versionCheckCode;
    if (httpCode == HTTP_CODE_OK) {
        if (versionCheckParsed) {
            currentMode = MODE_UPDATE_PAGE;
            updateDisplay();
        } else {
            otaErrorMessage = t(STR_OTA_ERR_PARSE);
            currentMode = MODE_OTA_ERROR;
            updateDisplay();
        }
    } else if (httpCode == 404) {
        // 서버에 해당 기기 펌웨어 미등록 → 현재 버전을 최신으로 처리
        logPrintf(LogLevel::LOG_WARN, "OTA: No firmware registered for this device (404). Treating as up-to-date.");
        otaState.latestVersion   = firmwareVersion;
        otaState.changeLog       = "";
        otaState.firmwareUrl     = "";
        otaState.updateAvailable = false;
        currentMode = MODE_UPDATE_PAGE;
        updateDisplay();
    } else {
        logPrintf(LogLevel::LOG_ERROR, "OTA: Version HTTP request failed: %d", httpCode);
        char errBuf[48];
        snprintf(errBuf, sizeof(errBuf), "%s (%d)", t(STR_OTA_ERR_CHECK), httpCode);
        otaErrorMessage = String(errBuf);
        currentMode = MODE_OTA_ERROR;
        updateDisplay();
    }
    otaVersionChecked = true;
    broadcastOtaStatus();
}

void performOtaUpdate() {
    if (otaState.inProgress) return;
    otaState.inProgress = true;
    otaState.downloadProgress = 0;
    logPrintf(LogLevel::LOG_INFO, "OTA: Starting firmware update...");
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (otaState.firmwareUrl.isEmpty()) {
        otaErrorMessage = t(STR_OTA_ERR_NO_URL);
        currentMode = MODE_OTA_ERROR;
        otaState.inProgress = false;
        updateDisplay();
        broadcastOtaResult(false, otaErrorMessage);
        return;
    }
    client.setTimeout(15);   // TLS 소켓 타임아웃 15초
    http.setTimeout(30000);  // 다운로드 HTTP 타임아웃 30초
    http.begin(client, otaState.firmwareUrl);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        if (contentLength > 0) {
            if (!Update.begin(contentLength)) {
                otaErrorMessage = t(STR_OTA_ERR_SPACE);
                currentMode = MODE_OTA_ERROR;
                otaState.inProgress = false;
                http.end();
                updateDisplay();
                broadcastOtaResult(false, otaErrorMessage);
                return;
            }
            WiFiClient *stream = http.getStreamPtr();
            size_t written = 0;
            uint8_t buff[1024] = {0};
            int lastProgress = -1;
            while (http.connected() && (written < (size_t)contentLength)) {
                size_t size = stream->available();
                if (size) {
                    size_t read = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
                    if (Update.write(buff, read) != read) {
                        otaErrorMessage = t(STR_OTA_ERR_WRITE);
                        currentMode = MODE_OTA_ERROR;
                        otaState.inProgress = false;
                        Update.abort();
                        http.end();
                        updateDisplay();
                        broadcastOtaResult(false, otaErrorMessage);
                        return;
                    }
                    written += read;

                    otaState.downloadProgress = (written * 100) / contentLength;

                    if (otaState.downloadProgress > lastProgress) {

                        broadcastOtaProgress(otaState.downloadProgress);

                        lastProgress = otaState.downloadProgress;

                    }

                    updateDisplay();

                }

                delay(1);

            }

            if (Update.end()) {

                logPrintf(LogLevel::LOG_INFO, "OTA: Update complete, waiting for exit to reboot...");

                otaUpdateDownloaded = true;

                currentMode = MODE_OTA_SUCCESS;

                broadcastOtaResult(true, "Download complete! Update will be applied on exit.");

            } else {

                Update.abort();

                otaErrorMessage = t(STR_OTA_ERR_UPDATE);

                currentMode = MODE_OTA_ERROR;

                broadcastOtaResult(false, otaErrorMessage);

            }

        } else {

            otaErrorMessage = t(STR_OTA_ERR_SIZE);

            currentMode = MODE_OTA_ERROR;

            broadcastOtaResult(false, otaErrorMessage);

        }

    } else {

        otaErrorMessage = t(STR_OTA_ERR_DL);

        currentMode = MODE_OTA_ERROR;

        broadcastOtaResult(false, otaErrorMessage);

    }

    http.end();

    otaState.inProgress = false;

    updateDisplay();

}



void sendResponse(AsyncWebServerRequest *request, int code, const String& contentType, const String& content) {
    AsyncWebServerResponse *response = request->beginResponse(code, contentType, content);
    response->addHeader("Connection", "close");
    request->send(response);
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        JsonDocument doc;
        getWifiStatusJson(doc);
        doc["type"] = "wifi_status_update";
        String output;
        serializeJson(doc, output);
        client->text(output);
        
        broadcastOtaStatus();
    }
}

void setupWebServer() {
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    server.on("/i18n.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "application/javascript", (const uint8_t*)I18N_JS, strlen_P(I18N_JS));
        response->addHeader("Connection", "close");
        request->send(response);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = getPageHeader("Nexus Transmitter", "pg_home");
        html += F("<div class='card'><h3 data-i18n='h_wifi_status'>Wi-Fi Status</h3><p id='home-wifi-status' data-i18n='w_loading'>Loading...</p></div>");
        html += F("<div class='card'><h3 data-i18n='h_dev_ctrl'>Device Control</h3>"
                  "<p><a href='/wifi' class='btn' data-i18n='h_btn_wifi'>Wi-Fi Settings</a></p>"
                  "<p><a href='/update' class='btn' data-i18n='h_btn_update'>Firmware Update</a></p>"
                  "<p><a href='/manual' class='btn btn-secondary' data-i18n='h_btn_manual'>User Manual</a></p>"
                  "<p><a href='/exit' class='btn btn-danger' data-i18n='h_btn_exit'>Exit Wi-Fi Mode</a></p>"
                  "</div>");
        html += getPageFooter(false);
        html += R"rawliteral(
        <script>
            let ws;
            function connectWs() {
                ws = new WebSocket("ws://"+window.location.host+"/ws");
                ws.onmessage = e => {
                    try {
                        const d = JSON.parse(e.data);
                        if (d.type === "wifi_status_update") {
                            let s = document.getElementById("home-wifi-status");
                            if (d.connected) {
                                s.innerHTML = wt('h_connected') + ": <b>" + escHtml(d.ssid) + "</b><br>" + wt('h_ip_addr') + " " + d.ip;
                            } else {
                                s.textContent = wt('h_not_conn');
                            }
                        }
                    } catch(err){}
                };
                ws.onopen = () => fetch("/api/wifi-status").then(res => res.json()).then(d => {
                    let s = document.getElementById("home-wifi-status");
                    if (d.connected) {
                        s.innerHTML = wt('h_connected') + ": <b>" + escHtml(d.ssid) + "</b><br>" + wt('h_ip_addr') + " " + d.ip;
                    } else {
                        s.textContent = wt('h_not_conn');
                    }
                });
                ws.onclose = () => setTimeout(connectWs, 2000);
            }
            window.onload = connectWs;
        </script>
        )rawliteral";
        sendResponse(request, 200, "text/html; charset=UTF-8", html);
    });

    server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = getPageHeader("Wi-Fi Settings", "pg_wifi");
        html += R"rawliteral(
        <div class="card">
            <h2 data-i18n="w_cur_status">Current Wi-Fi Status</h2>
            <p id="conn-status" data-i18n="w_loading">Loading...</p>
        </div>
        <div class="card">
            <h2 data-i18n="w_conn_card">Wi-Fi Connection</h2>
            <div class="form-group" style="text-align: center;">
                <label for="ssid-select" data-i18n="w_sel_ssid">Select SSID:</label>
                <select id="ssid-select" class="form-control">
                    <option value="" data-i18n="w_scan_ph">-- Scan to select a network --</option>
                </select>
            </div>
            <div style="display: flex; justify-content: center; margin-top: 20px; gap: 10px;">
                <button id="scan-btn" class="btn" onclick="scanWifi()" data-i18n="w_rescan">Rescan</button>
            </div>
            <div class="form-group" style="text-align: center;">
                <label for="password-input" data-i18n="w_pass">Password:</label>
                <input type="password" id="password-input" class="form-control">
            </div>
            <button id="action-btn" class="btn" onclick="handleConnectDisconnect()" data-i18n="w_connect">Connect</button>
        </div>
        <script>
            const scanBtn = document.getElementById("scan-btn");
            const actionBtn = document.getElementById("action-btn");
            const ssidSelect = document.getElementById("ssid-select");
            const passwordInput = document.getElementById("password-input");
            const connStatusEl = document.getElementById("conn-status");
            let ws, currentSsid = '', isConnected = false;
            
            function connectWs() {
                ws = new WebSocket("ws://" + location.host + "/ws");
                ws.onopen = () => { 
                    console.log('WebSocket Connected'); 
                    fetchStatus(); 
                };
                ws.onclose = () => { 
                    console.log('WebSocket Disconnected, reconnecting...'); 
                    setTimeout(connectWs, 2000); 
                };
                ws.onmessage = evt => {
                    try {
                        const data = JSON.parse(evt.data);
                        if (data.type === "wifi_status_update") handleWifiStatus(data);
                        if (data.type === "scan_result") handleScanResult(data);
                    } catch (e) { 
                        console.error("WS Parse Error:", e); 
                    }
                };
            }

            function fetchStatus() { 
                fetch("/api/wifi-status")
                    .then(response => response.json())
                    .then(data => handleWifiStatus(data))
                    .catch(err => console.error('Error fetching status:', err));
            }

            function handleConnectDisconnect() {
                if (isConnected) { disconnectWifi(); } else { connectWifi(); }
            }

            function handleWifiStatus(data) {
                currentSsid = data.connected ? data.ssid : '';
                isConnected = data.connected;

                actionBtn.disabled = false;
                scanBtn.disabled = false;
                passwordInput.disabled = false;
                ssidSelect.disabled = false;
                actionBtn.classList.remove('btn-danger', 'btn-success');

                if (data.connected) {
                    connStatusEl.innerHTML = wt('h_connected') + ' <b>' + escHtml(data.ssid) + '</b> (IP: ' + data.ip + ')';
                    actionBtn.textContent = wt('w_disconnect');
                    actionBtn.classList.add('btn-danger');
                    passwordInput.disabled = true;
                    ssidSelect.disabled = true;
                    scanBtn.disabled = true;
                    if (data.status === "connected") { showMessage(wt('msg_connected_ok'), 'success'); }
                } else {
                    connStatusEl.textContent = wt('msg_not_conn_s');
                    actionBtn.textContent = wt('w_connect');
                    actionBtn.classList.add('btn');
                    switch (data.status) {
                        case "connecting":
                            showMessage(wt('msg_connecting'), 'info', 0);
                            actionBtn.disabled = true;
                            scanBtn.disabled = true;
                            passwordInput.disabled = true;
                            ssidSelect.disabled = true;
                            break;
                        case "failed":
                            actionBtn.disabled = false;
                            scanBtn.disabled = false;
                            passwordInput.disabled = false;
                            ssidSelect.disabled = false;
                            if (data.reason === 15 || data.reason === 2 || data.reason === 8) {
                                showMessage(wt('msg_conn_fail'), 'error');
                            } else if (data.reason === 201) {
                                showMessage(wt('msg_conn_fail') + " (Out of range)", 'error');
                            } else {
                                showMessage(wt('msg_conn_fail') + ' (R:' + data.reason + ')', 'error');
                            }
                            break;
                        case "disconnected":
                            showMessage(wt('msg_disconnected'), 'info');
                            break;
                        default:
                            break;
                    }
                }
            }

            function scanWifi() {
                if (scanBtn.disabled) return;
                scanBtn.disabled = true;
                ssidSelect.innerHTML = "<option>" + wt('msg_scanning') + "</option>";
                showMessage(wt('msg_scanning'), "info");
                fetch("/api/scan-wifi").catch(() => {
                    showMessage(wt('msg_scan_fail'), "error");
                    scanBtn.disabled = false;
                });
            }

            function handleScanResult(data) {
                ssidSelect.innerHTML = "<option value=''>" + wt('msg_sel_net_ph') + "</option>";
                if (data.networks && data.networks.length > 0) {
                    data.networks.slice(0, 20).forEach(net => {
                        const lockIcon = net.encrypted ? "🔒" : " ";
                        const option = new Option(lockIcon + ' ' + net.ssid + ' (' + net.rssi + ' dBm)', net.ssid);
                        ssidSelect.add(option);
                    });
                    showMessage(wt('msg_scan_ok'), "success");
                } else {
                    ssidSelect.innerHTML = "<option>" + wt('msg_no_nets') + "</option>";
                    showMessage(wt('msg_no_nets'), "error");
                }
                scanBtn.disabled = false;
            }

            function connectWifi() {
                const ssid = ssidSelect.value;
                if (!ssid) { showMessage(wt('msg_select_net'), "error"); return; }
                actionBtn.disabled = true;
                scanBtn.disabled = true;
                passwordInput.disabled = true;
                ssidSelect.disabled = true;
                showMessage(wt('msg_connecting'), "info", 0);
                const password = passwordInput.value;
                fetch("/api/connect-wifi", {
                    method: "POST",
                    headers: { "Content-Type": "application/x-www-form-urlencoded" },
                    body: "ssid=" + encodeURIComponent(ssid) + "&password=" + encodeURIComponent(password)
                }).catch(() => {
                    showMessage(wt('msg_conn_fail'), "error");
                    actionBtn.disabled = false;
                    scanBtn.disabled = false;
                    passwordInput.disabled = false;
                    ssidSelect.disabled = false;
                });
            }

            function disconnectWifi() {
                if (!window.confirm(wt('msg_confirm_disc'))) {
                    return;
                }
                actionBtn.disabled = true;
                showMessage(wt('msg_disconnecting'), 'info', 0);
                fetch("/api/disconnect-wifi", {
                    method: "POST",
                    headers: { "Content-Type": "application/x-www-form-urlencoded" },
                    body: "ssid=" + encodeURIComponent(currentSsid)
                }).then(() => {
                    passwordInput.value = '';
                }).catch(() => {
                    showMessage(wt('msg_conn_fail'), 'error');
                });
            }

            window.onload = () => {
                connectWs();
                fetchStatus();
                setTimeout(scanWifi, 500);
            };
        </script>
        )rawliteral";

        html += getPageFooter(true);

        sendResponse(request, 200, "text/html; charset=UTF-8", html);

    });



    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {

        String html = getPageHeader("Firmware Update", "pg_update");

        html += R"rawliteral(

        <div class='card'>

            <p><span data-i18n='u_cur_ver'>Current Version:</span> <b id='current-v'>-</b><br>

               <span data-i18n='u_lat_ver'>Latest on Server:</span> <b id='latest-v'>-</b></p>

            <div id='update-info'>

                 <div id='changelog' class='changelog'></div>

                 <p id='update-status'></p>

            </div>

            <div class="form-group" style="text-align: center;">

                <button id='update-btn' class='btn hidden' onclick='downloadUpdate()' data-i18n='u_btn'>Update</button>

                <div id='download-progress' class='hidden' style='margin-top: 10px; display: flex; flex-direction: column; align-items: center;'>

                    <span id='progress-text' style='font-weight: bold;'>0%</span>

                    <div class='progress-bar'>

                        <div class='progress-bar-inner' id='progress-bar-inner'></div>

                    </div>

                </div>

                <p id='download-notice' class='hidden notice'></p>

            </div>

        </div>

        <script>
            const updateBtn = document.getElementById('update-btn');
            const updateStatus = document.getElementById('update-status');
            const changelogEl = document.getElementById('changelog');
            const downloadNotice = document.getElementById('download-notice');
            const downloadProgressDiv = document.getElementById('download-progress');
            const progressBarInner = document.getElementById('progress-bar-inner');
            const progressText = document.getElementById('progress-text');
            let ws;

            function connectWs() {
                ws = new WebSocket("ws://"+window.location.host+"/ws");
                ws.onmessage = e => {
                    try {
                        const d = JSON.parse(e.data);
                        if (d.type === "ota_status") updateOtaUi(d);
                        if (d.type === "ota_progress") updateProgressText(d.progress);
                        if (d.type === "ota_result") handleOtaResult(d);
                        if (d.type === "wifi_status_update") fetch("/api/check-ota");
                    } catch(err) { console.error("OTA WS Error:", err); }
                };
                ws.onopen = () => fetch("/api/check-ota");
                ws.onclose = () => setTimeout(connectWs, 2000);
            }

            function updateOtaUi(d) {
                document.getElementById("current-v").textContent = d.current_version;
                document.getElementById("latest-v").textContent = d.latest_version;

                if (!d.internet_ok) {
                    updateStatus.innerHTML = "<span class='message-error'>" + wt('msg_wifi_req') + "</span>";
                    changelogEl.textContent = wt('msg_check_fail');
                    updateBtn.classList.add("hidden");
                    downloadProgressDiv.classList.add('hidden');
                    return;
                }

                changelogEl.textContent = d.check_ok ? (d.changelog || '') : wt('msg_check_fail');

                if (d.update_available) {
                    updateStatus.innerHTML = "<b style='color:#34d399;'>" + wt('msg_update_avail') + "</b>";
                    updateBtn.classList.remove("hidden");
                    updateBtn.textContent = wt('u_btn');
                    updateBtn.disabled = false;
                    downloadProgressDiv.classList.add('hidden');
                } else {
                    updateStatus.textContent = wt('msg_up2date');
                    updateBtn.classList.add("hidden");
                    downloadProgressDiv.classList.add('hidden');
                }
            }

            function downloadUpdate() {
                if (!window.confirm(wt('msg_confirm_upd'))) return;
                updateBtn.disabled = true;
                downloadNotice.classList.remove("hidden");
                downloadProgressDiv.classList.remove('hidden');
                updateProgressText(0);
                fetch("/api/download-ota", { method: "POST" });
            }

            function updateProgressText(progress) {
                progressText.textContent = progress + "%";
                progressBarInner.style.width = progress + "%";
            }

            function handleOtaResult(d) {
                showMessage(d.msg, d.success ? 'success' : 'error');
                if (d.success) {
                    updateBtn.textContent = wt('u_btn_done');
                    updateBtn.classList.remove("btn");
                    updateBtn.classList.add('btn-success');
                    downloadProgressDiv.classList.add('hidden');
                } else {
                    updateBtn.textContent = wt('u_btn');
                    updateBtn.disabled = false;
                    downloadProgressDiv.classList.add('hidden');
                }
            }

            window.onload = connectWs;
        </script>

        )rawliteral";

        html += getPageFooter(true);

        sendResponse(request, 200, "text/html; charset=UTF-8", html);

    });



    server.on("/manual", HTTP_GET, [](AsyncWebServerRequest *request) {

        struct ManualStreamState {

            String header;

            const char* body;

            size_t bodyLen;

            String footer;

            int section = 0;

            size_t offset = 0;

        };



        static const char MANUAL_BODY[] PROGMEM = R"rawliteral(
<style>
details{text-align:left;margin-bottom:12px;border-radius:12px;border:1px solid rgba(255,255,255,0.08);overflow:hidden;}
summary{cursor:pointer;padding:14px 18px;font-weight:600;font-size:15px;color:#e5e7eb;background:rgba(255,255,255,0.03);list-style:none;display:flex;align-items:center;gap:8px;}
summary::-webkit-details-marker{display:none;}
summary::before{content:'▶';font-size:11px;color:#a78bfa;transition:transform 0.2s;flex-shrink:0;}
details[open] summary::before{transform:rotate(90deg);}
details[open] summary{color:#a78bfa;}
.mb{padding:14px 16px;font-size:14px;line-height:1.7;color:#d1d5db;border-top:1px solid rgba(255,255,255,0.05);overflow-x:auto;-webkit-overflow-scrolling:touch;}
.kb{display:inline-block;background:rgba(167,139,250,0.15);border:1px solid rgba(167,139,250,0.3);color:#c4b5fd;border-radius:6px;padding:2px 8px;font-size:12px;font-weight:600;font-family:monospace;white-space:nowrap;}
.sl{margin:8px 0;padding-left:0;list-style:none;counter-reset:sc;}
.sl li{counter-increment:sc;display:flex;gap:10px;margin-bottom:8px;align-items:flex-start;}
.sl li::before{content:counter(sc);background:#6d28d9;color:white;border-radius:50%;width:20px;height:20px;font-size:11px;font-weight:700;display:flex;align-items:center;justify-content:center;flex-shrink:0;margin-top:2px;}
.tb{background:rgba(5,150,105,0.1);border:1px solid rgba(5,150,105,0.3);border-radius:8px;padding:10px 14px;margin-top:10px;font-size:13px;color:#6ee7b7;}
.wb{background:rgba(220,38,38,0.1);border:1px solid rgba(220,38,38,0.3);border-radius:8px;padding:10px 14px;margin-top:10px;font-size:13px;color:#fca5a5;}
.op{background:#000;border:2px solid #444;border-radius:6px;padding:8px 10px;font-family:monospace;font-size:12px;color:#fff;margin:8px 0;line-height:1.4;white-space:pre;overflow-x:auto;}
.st{width:100%;border-collapse:collapse;font-size:13px;margin-top:8px;}
.st th{background:rgba(109,40,217,0.3);color:#c4b5fd;padding:7px 8px;text-align:left;font-weight:600;}
.st td{padding:6px 8px;border-top:1px solid rgba(255,255,255,0.05);color:#d1d5db;word-break:break-word;overflow-wrap:break-word;}
.st tr:nth-child(even) td{background:rgba(255,255,255,0.02);}
.st td:first-child,.st th:first-child{white-space:nowrap;text-align:center;padding-left:6px;padding-right:6px;}
</style>
<div class='card' style='text-align:left;'>
<details open><summary data-i18n='m_s1'>🎮 버튼 조작법</summary><div class='mb' data-i18n-html='m_s1_body'></div></details>
<details><summary data-i18n='m_s2'>🏠 홈 메뉴</summary><div class='mb' data-i18n-html='m_s2_body'></div></details>
<details><summary data-i18n='m_s3'>🎯 재생/설정</summary><div class='mb' data-i18n-html='m_s3_body'></div></details>
<details><summary data-i18n='m_s4'>💥 그룹 재생</summary><div class='mb' data-i18n-html='m_s4_body'></div></details>
<details><summary data-i18n='m_s5'>📡 자동 채널 선택</summary><div class='mb' data-i18n-html='m_s5_body'></div></details>
<details><summary data-i18n='m_s6'>🔗 기기 페어링</summary><div class='mb' data-i18n-html='m_s6_body'></div></details>
<details><summary data-i18n='m_s7'>📋 예비 기기 복사</summary><div class='mb' data-i18n-html='m_s7_body'></div></details>
<details><summary data-i18n='m_s8'>🔄 펌웨어 업데이트</summary><div class='mb' data-i18n-html='m_s8_body'></div></details>
<details><summary data-i18n='m_s9'>⚙️ 기타 설정</summary><div class='mb' data-i18n-html='m_s9_body'></div></details>
<details><summary data-i18n='m_s10'>📊 OLED 실행 상태</summary><div class='mb' data-i18n-html='m_s10_body'></div></details>
<details><summary data-i18n='m_s11'>💡 마술 연출 가이드</summary><div class='mb' data-i18n-html='m_s11_body'></div></details>
</div>
)rawliteral";




        auto state = std::make_shared<ManualStreamState>();

        state->header = getPageHeader("사용 설명서", "pg_manual");

        state->body = MANUAL_BODY;

        state->bodyLen = strlen_P(MANUAL_BODY);

        state->footer = getPageFooter(true);

        state->section = 0;

        state->offset = 0;



        AsyncWebServerResponse *response = request->beginChunkedResponse("text/html; charset=UTF-8", [state](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {

            if (state->section == 0) {

                size_t len = state->header.length();

                size_t available = len - state->offset;

                size_t toWrite = (available > maxLen) ? maxLen : available;

                if (toWrite > 0) {

                    memcpy(buffer, state->header.c_str() + state->offset, toWrite);

                    state->offset += toWrite;

                    return toWrite;

                }

                state->section = 1;

                state->offset = 0;

            }

            if (state->section == 1) {

                size_t available = state->bodyLen - state->offset;

                size_t toWrite = (available > maxLen) ? maxLen : available;

                if (toWrite > 0) {

                    memcpy_P(buffer, state->body + state->offset, toWrite);

                    state->offset += toWrite;

                    return toWrite;

                }

                state->section = 2;

                state->offset = 0;

            }

            if (state->section == 2) {

                size_t len = state->footer.length();

                size_t available = len - state->offset;

                size_t toWrite = (available > maxLen) ? maxLen : available;

                if (toWrite > 0) {

                    memcpy(buffer, state->footer.c_str() + state->offset, toWrite);

                    state->offset += toWrite;

                    return toWrite;

                }

                state->section = 3;

            }

            return 0;

        });



        response->addHeader("Connection", "close");
        request->send(response);

    });

    server.on("/exit", HTTP_GET, [](AsyncWebServerRequest *request) {

        String html = getPageHeader("Exiting Wi-Fi Mode", "pg_exit");

        html += "<p data-i18n='exit_msg'>The device will now return to normal operation. You can close this window.</p>";

        if (otaUpdateDownloaded) {

            html += "<p style='background:rgba(5,150,105,0.15);border:1px solid rgba(5,150,105,0.4);border-radius:8px;padding:10px 14px;color:#6ee7b7;font-weight:600;'>✅ 업데이트가 다운로드됐습니다. 재시작 후 자동 적용됩니다.</p>";

        }

        html += getPageFooter(false);

        sendResponse(request, 200, "text/html; charset=UTF-8", html);

        // 응답 전송이 완료되기 전에 WiFi 스택을 종료하면 응답이 유실될 수 있음.

        // → 플래그만 세우고 handleOtaLoop()(Core 1)에서 shutdownWifi+initEspNow 처리.

        if (otaUpdateDownloaded) {

            otaRestartRequest = true;

        } else {

            pendingOtaExit = true;

        }

    });



    server.on("/api/wifi-status", HTTP_GET, [](AsyncWebServerRequest *request){

        String statusStr = "idle";

        if (otaWifiStatus == OTA_WIFI_CONNECTING)    statusStr = "connecting";

        else if (otaWifiStatus == OTA_WIFI_CONNECTED)    statusStr = "connected";

        else if (otaWifiStatus == OTA_WIFI_FAILED)       statusStr = "failed";

        else if (otaWifiStatus == OTA_WIFI_DISCONNECTED) statusStr = "disconnected";



        JsonDocument doc;

        doc["status"] = statusStr;

        doc["connected"] = (otaWifiStatus == OTA_WIFI_CONNECTED);

        if (otaWifiStatus == OTA_WIFI_CONNECTED) {

            doc["ssid"] = WiFi.SSID();

            doc["ip"] = WiFi.localIP().toString();

        }

        String responseStr;

        serializeJson(doc, responseStr);

        sendResponse(request, 200, "application/json", responseStr);

    });



    server.on("/api/scan-wifi", HTTP_GET, [](AsyncWebServerRequest *request){

        if (!scan_in_progress) {

            scan_in_progress = true;

            BaseType_t res = xTaskCreate(wifiScanTask, "wifiScanTask", 8192, nullptr, 5, NULL);

            if (res != pdPASS) {

                scan_in_progress = false;

                sendResponse(request, 500, "application/json", "{\"error\":\"Memory allocation failed\"}");

            } else {

                sendResponse(request, 200, "application/json", "{\"status\":\"Scan initiated\"}");

            }

        } else {

            sendResponse(request, 503, "application/json", "{\"error\":\"Scan already in progress\"}");

        }

    });



    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request){

        JsonDocument doc;

        JsonArray networks = doc["networks"].to<JsonArray>();

        // Take mutex before reading cachedScanResults — wifiScanTask writes from another context.

        if (scanResultsMutex && xSemaphoreTake(scanResultsMutex, pdMS_TO_TICKS(200)) == pdTRUE) {

            for (const auto& net : cachedScanResults) {
                JsonObject obj = networks.add<JsonObject>();
                obj["ssid"] = net.ssid;
                obj["rssi"] = net.rssi;
                obj["encrypted"] = net.encrypted;
            }

            xSemaphoreGive(scanResultsMutex);

        }

        doc["in_progress"] = (bool)scan_in_progress;

        String responseStr;

        serializeJson(doc, responseStr);

        sendResponse(request, 200, "application/json", responseStr);

    });



    server.on("/connect", HTTP_POST, [](AsyncWebServerRequest *request){
        String ssid, pass;
        if (request->hasParam("ssid", true)) ssid = request->getParam("ssid", true)->value();
        if (request->hasParam("pass", true)) pass = request->getParam("pass", true)->value();
        else if (request->hasParam("password", true)) pass = request->getParam("password", true)->value();

        if (ssid.length() > 0 && ssid.length() < EEPROM_WIFI_SSID_SIZE && pass.length() < EEPROM_WIFI_PASS_SIZE) {
            wifi_password = pass;
            otaConnectingSsid = ssid;
            WiFi.disconnect(false);
            vTaskDelay(pdMS_TO_TICKS(200));
            otaWifiStatus = OTA_WIFI_CONNECTING;
            wifiConnectStartMillis = millis();
            pendingOtaMode = 3; // → transition to MODE_OTA_CONNECTING
            WiFi.begin(ssid.c_str(), pass.c_str());
            sendResponse(request, 200, "application/json", "{\"success\":true}");
        } else {
            sendResponse(request, 400, "application/json", "{\"success\":false}");
        }
    });

    server.on("/api/connect-wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        String ssid, pass;
        if (request->hasParam("ssid", true)) ssid = request->getParam("ssid", true)->value();
        if (request->hasParam("pass", true)) pass = request->getParam("pass", true)->value();
        else if (request->hasParam("password", true)) pass = request->getParam("password", true)->value();

        if (ssid.length() > 0 && ssid.length() < EEPROM_WIFI_SSID_SIZE && pass.length() < EEPROM_WIFI_PASS_SIZE) {
            wifi_password = pass;
            otaConnectingSsid = ssid;
            WiFi.disconnect(false);
            vTaskDelay(pdMS_TO_TICKS(200));
            otaWifiStatus = OTA_WIFI_CONNECTING;
            wifiConnectStartMillis = millis();
            pendingOtaMode = 3; // → transition to MODE_OTA_CONNECTING
            WiFi.begin(ssid.c_str(), pass.c_str());
            sendResponse(request, 200, "application/json", "{\"success\":true}");
        } else {
            sendResponse(request, 400, "application/json", "{\"success\":false}");
        }
    });

    server.on("/disconnect", HTTP_POST, [](AsyncWebServerRequest *request){
        String ssid;
        if (request->hasParam("ssid", true)) ssid = request->getParam("ssid", true)->value();
        if (ssid.length() > 0) {
            removeKnownNetwork(ssid);
        }
        WiFi.disconnect(true, true);
        otaWifiStatus = OTA_WIFI_DISCONNECTED;
        otaWifiDisconnectFlag = true;
        sendResponse(request, 200, "application/json", "{\"success\":true}");
    });

    server.on("/api/disconnect-wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        String ssid;
        if (request->hasParam("ssid", true)) ssid = request->getParam("ssid", true)->value();
        if (ssid.length() > 0) {
            removeKnownNetwork(ssid);
        }
        WiFi.disconnect(true, true);
        otaWifiStatus = OTA_WIFI_DISCONNECTED;
        otaWifiDisconnectFlag = true;
        sendResponse(request, 200, "application/json", "{\"success\":true}");
    });



    server.on("/api/check-ota", HTTP_GET, [](AsyncWebServerRequest *request){

        checkOtaFromApiFlag = true;

        sendResponse(request, 200, "application/json", "{\"status\":\"checking\"}");

    });



    server.on("/api/download-ota", HTTP_POST, [](AsyncWebServerRequest *request){

        otaStartDownloadFlag = true;

        sendResponse(request, 200, "application/json", "{\"status\":\"download_started\"}");

    });



    server.on("/api/ota-status", HTTP_GET, [](AsyncWebServerRequest *request){

        JsonDocument doc;

        doc["current_version"] = firmwareVersion;

        doc["latest_version"] = otaState.latestVersion.isEmpty() ? "" : otaState.latestVersion;

        doc["update_available"] = otaState.updateAvailable;

        doc["in_progress"] = otaState.inProgress;

        doc["progress"] = otaState.downloadProgress;

        doc["changelog"] = otaState.changeLog.isEmpty() ? "" : otaState.changeLog;

        doc["error"] = otaErrorMessage;

        doc["downloaded"] = otaUpdateDownloaded;

        // check_done: true when the version check has completed (success or failure)

        doc["check_done"] = !otaState.latestVersion.isEmpty() || !otaErrorMessage.isEmpty();

        String responseStr;

        serializeJson(doc, responseStr);

        sendResponse(request, 200, "application/json", responseStr);

    });



    server.on("/api/set-language", HTTP_POST, [](AsyncWebServerRequest *request){

        if (request->hasParam("lang", true)) {

            int lang = request->getParam("lang", true)->value().toInt();

            if (lang >= 0 && lang < (int)LANG_COUNT) setLanguage((Language)lang);

        }

        sendResponse(request, 200, "application/json", "{\"ok\":true}");

    });

}
