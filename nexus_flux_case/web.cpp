/**
 * @file web.cpp
 * @brief WebManager Class Implementation. Manages AP mode, web server, API, and OTA.
 * @version 8.4.0
 * @date 2024-06-17
 */

#include "web.h"
#include "mode.h"
#include "comm.h"
#include "utils.h"
#include <vector>
#include <algorithm>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <memory>

// External declaration for global instance defined in .ino file
// WebManager webManager;

// WebManager의 static 인스턴스 포인터 초기화
WebManager* WebManager::_instance = nullptr;


static const char I18N_JS[] PROGMEM = R"rawliteral(

        const _LC=['EN','한','中','日','DE','ES','FR'];
        function _getLi(){const r=localStorage.getItem('nx_lang');if(r==='ko')return 1;if(r==='en')return 0;const n=parseInt(r,10);return isNaN(n)?0:n;}
        function setLang(i){localStorage.setItem('nx_lang',i);location.reload();}
        function toggleLangMenu(e){e.stopPropagation();document.getElementById('lang-sw').classList.toggle('open');}
        document.addEventListener('click',function(e){const sw=document.getElementById('lang-sw');if(sw&&!sw.contains(e.target))sw.classList.remove('open');});
        window.LANG_IDX=_getLi();
        const I18N={
back_home:["Back to Home","홈으로 돌아가기","返回主页","ホームに戻る","Zurück","Volver","Retour"],
pg_manual:["User Manual","사용 설명서","用户手册","取扱説明書","Benutzerhandbuch","Manual de usuario","Manuel d'utilisation"],
h_btn_manual:["User Manual","사용 설명서","用户手册","取扱説明書","Benutzerhandbuch","Manual","Manuel"],
pg_home:["Nexus Flux Case","Nexus 플럭스 케이스","Nexus 通量盒","Nexus フラックスケース","Nexus Flux-Gehäuse","Nexus Caja Flux","Nexus Boîtier Flux"],
pg_wifi:["Wi-Fi Settings","Wi-Fi 설정","Wi-Fi 设置","Wi-Fi 設定","WLAN Einstellungen","Ajustes Wi-Fi","Paramètres Wi-Fi"],
pg_update:["Firmware Update","펌웨어 업데이트","固件更新","ファームウェア更新","Firmware-Update","Actualización Firmware","Mise à jour Firmware"],
pg_test:["Test Mode","테스트 모드","测试模式","テストモード","Testmodus","Modo de prueba","Mode de test"],
pg_exit:["Exiting Wi-Fi Mode","Wi-Fi 모드 종료","退出 Wi-Fi 模式","Wi-Fiモード終了","WLAN-Modus beenden","Salir Modo Wi-Fi","Sortie Mode Wi-Fi"],
h_wifi_status:["Wi-Fi Status","Wi-Fi 상태","Wi-Fi 状态","Wi-Fi 状態","WLAN Status","Estado Wi-Fi","État Wi-Fi"],
h_dev_ctrl:["Device Control","기기 제어","设备控制","機器制御","Gerätesteuerung","Control del Dispositivo","Contrôle Appareil"],
h_btn_wifi:["Wi-Fi Settings","Wi-Fi 설정","Wi-Fi 设置","Wi-Fi 設定","WLAN Einstellungen","Ajustes Wi-Fi","Paramètres Wi-Fi"],
h_btn_update:["Firmware Update","펌웨어 업데이트","固件更新","ファームウェア更新","Firmware-Update","Actualizar Firmware","Mise à jour"],
h_btn_test:["Test Mode","테스트 모드","测试模式","テストモード","Testmodus","Modo de prueba","Mode de test"],
h_btn_exit:["Exit Wi-Fi Mode","Wi-Fi 모드 종료","退出 Wi-Fi 模式","Wi-Fiモード終了","WLAN beenden","Salir Wi-Fi","Quitter Wi-Fi"],
h_not_conn:["Not connected. AP Mode active.","연결 안됨. AP 모드 활성","未连接，AP模式激活","未接続。APモード有効","Nicht verbunden. AP Modus","No conectado. Modo AP","Non connecté. Mode AP"],
h_connected:["Connected to","연결됨:","已连接:","接続済:","Verbunden mit","Conectado a","Connecté à"],
h_ip_addr:["IP:","IP:","IP:","IP:","IP:","IP:","IP:"],
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
exit_msg:["The device will now return to normal operation. You can close this window.","기기가 일반 모드로 돌아갑니다. 이 창을 닫아도 됩니다.","设备即将返回正常操作，可关闭此窗口。","デバイスは通常操作に戻ります。このウィンドウを閉じてください。","Gerät kehrt in Normalbetrieb zurück.","El dispositivo vuelve al modo normal.","L'appareil revient en mode normal."],
exit_ota_msg:["An update was downloaded and will be applied on reboot.","업데이트가 다운로드되었으며 재부팅 시 적용됩니다.","更新已下载，将在重启后应用。","アップデートがダウンロードされ、再起動後に適用されます。","Update heruntergeladen und wird nach Neustart angewendet.","La actualización fue descargada y se aplicará al reiniciar.","La mise à jour a été téléchargée et sera appliquée au redémarrage."],
dev_settings:["Device Settings","기기 설정","设备设置","デバイス設定","Geräteeinstellungen","Config. Dispositivo","Paramètres Appareil"],
dev_id_label:["Device ID :","기기 ID :","设备 ID :","デバイス ID :","Gerät-ID :","ID Dispositivo :","ID Appareil :"],
save_btn:["Save","저장","保存","保存","Speichern","Guardar","Enregistrer"],
delay_label:["Delay Timer (s) :","딜레이 (초) :","延迟 (秒) :","遅延 (秒) :","Verzögerung (s) :","Retardo (s) :","Délai (s) :"],
play_label:["Play Timer (s) :","실행 시간 (초) :","执行时间 (秒) :","実行時間 (秒) :","Spielzeit (s) :","Tiempo (s) :","Durée (s) :"],
run_btn:["Run Manual Test","수동 테스트 실행","手动测试","手動テスト実行","Manueller Test","Prueba Manual","Test Manuel"],
running_btn:["Running...","실행 중...","执行中...","実行中...","Läuft...","Ejecutando...","En cours..."],
log_title:["Live Log","실시간 로그","实时日志","ライブログ","Live-Log","Registro en Vivo","Journal en Direct"],
log_clear:["Clear","지우기","清除","クリア","Löschen","Limpiar","Effacer"],
no_remote_warn:["This mode does not support connection with the transmitter.<br>Communication will be enabled when you exit this mode.","이 모드에서는 리모컨과의 통신이 지원되지 않습니다.<br>모드 종료 후 정상 통신이 활성화됩니다.","此模式不支持与发射器连接。<br>退出此模式后，通信将恢复正常。","このモードでは送信機との通信はサポートされません。<br>モード終了後に通信が有効になります。","Dieser Modus unterstützt keine Verbindung zum Sender.<br>Nach dem Beenden wird die Kommunikation aktiviert.","Este modo no admite conexión con el transmisor.<br>La comunicación se habilitará al salir.","Ce mode ne prend pas en charge la connexion à l'émetteur.<br>La communication sera activée à la sortie."],
msg_scanning:["Scanning for Wi-Fi networks...","Wi-Fi 네트워크 스캔 중...","扫描 Wi-Fi 网络...","Wi-Fi検索中...","WLAN suchen...","Buscando redes...","Recherche réseaux..."],
msg_scanning_opt:["Scanning...","스캔 중...","扫描中...","スキャン中...","Suche...","Buscando...","Recherche..."],
msg_scan_ok:["Scan complete. Select a network.","스캔 완료. 네트워크를 선택하세요.","扫描完成，请选择网络。","スキャン完了。ネットワークを選択してください。","Scan abgeschlossen. Netz wählen.","Escaneo completo. Elige red.","Scan terminé. Sélectionnez un réseau."],
msg_no_nets:["No Wi-Fi networks found.","Wi-Fi 네트워크를 찾을 수 없습니다.","未找到 Wi-Fi 网络。","Wi-Fiネットワークが見つかりません。","Keine WLAN-Netzwerke gefunden.","No se encontraron redes Wi-Fi.","Aucun réseau Wi-Fi trouvé."],
msg_scan_fail:["Failed to start scan.","스캔 시작 실패.","启动扫描失败。","スキャン開始に失敗。","Scan konnte nicht gestartet werden.","Error al iniciar el escaneo.","Impossible de lancer le scan."],
msg_connecting:["Attempting to connect...","연결 시도 중...","尝试连接...","接続試行中...","Verbinde...","Intentando conectar...","Connexion en cours..."],
msg_conn_ok:["Wi-Fi connection successful!","Wi-Fi 연결됨!","Wi-Fi 已连接！","Wi-Fi 接続完了！","WLAN verbunden!","¡Wi-Fi conectado!","Wi-Fi connecté!"],
msg_conn_fail:["Connection failed. Please check your password.","연결 실패. 비밀번호 확인","连接失败，检查密码","接続失敗。パスワード確認","Verbindung fehlgeschlagen","Conexión fallida","Connexion échouée"],
msg_conn_fail_range:["Connection failed. The network is out of range or not found.","연결 실패. 네트워크를 찾을 수 없습니다.","连接失败。网络超出范围或未找到。","接続失敗。ネットワークが範囲外または見つかりません。","Verbindung fehlgeschlagen. Netz nicht in Reichweite.","Fallo: red fuera de rango o no encontrada.","Échec: réseau hors portée ou introuvable."],
msg_conn_fail_env:["Connection failed. Environment may be unstable.","연결 실패. 환경이 불안정할 수 있습니다.","连接失败。环境可能不稳定。","接続失敗。環境が不安定な可能性があります。","Verbindung fehlgeschlagen. Umgebung könnte instabil sein.","Fallo: el entorno puede ser inestable.","Échec: l'environnement peut être instable."],
msg_disconnecting:["Disconnecting...","연결 해제 중...","正在断开...","切断中...","Trennen...","Desconectando...","Déconnexion..."],
msg_disconnected:["Disconnected from Wi-Fi.","Wi-Fi 연결 해제됨","已断开 Wi-Fi","Wi-Fi切断","WLAN getrennt","Wi-Fi desconectado","Wi-Fi déconnecté"],
msg_not_conn_s:["Not Connected","미연결","未连接","未接続","Nicht verbunden","No conectado","Non connecté"],
msg_confirm_disc:["Are you sure you want to disconnect? The saved password for this network will be removed.","연결을 끊겠습니까? 이 네트워크의 저장된 비밀번호가 삭제됩니다.","确认断开？该网络的保存密码将被删除。","接続を切断しますか？このネットワークの保存済みパスワードが削除されます。","Verbindung trennen? Gespeichertes Passwort wird gelöscht.","¿Desconectar? La contraseña guardada se eliminará.","Déconnecter? Le mot de passe enregistré sera supprimé."],
msg_select_net:["Please select a network first.","먼저 네트워크를 선택하세요","请先选择网络","ネットワークを選択してください","Bitte Netz wählen","Seleccione una red","Sélectionner un réseau"],
msg_sel_net_ph:["-- Select a Network --","-- 네트워크 선택 --","-- 选择网络 --","-- ネットワーク選択 --","-- Netz wählen --","-- Selec. red --","-- Sélec. réseau --"],
msg_disc_fail:["Failed to send disconnect request.","연결 해제 요청 실패.","发送断开请求失败。","切断リクエストの送信に失敗。","Trennungsanfrage fehlgeschlagen.","Error al enviar solicitud de desconexión.","Échec de la demande de déconnexion."],
msg_up2date:["You are on the latest version.","최신 버전입니다.","已是最新版本","最新バージョンです","Neueste Version vorhanden","Versión más reciente","Version la plus récente"],
msg_update_avail:["Update available!","업데이트 사용 가능!","有可用更新！","アップデートあり！","Update verfügbar!","¡Actualización disponible!","Mise à jour disponible!"],
msg_update_done:["Download complete! Update will be applied on exit.","다운로드 완료! Wi-Fi 종료 후 적용됩니다.","下载完成！退出Wi-Fi后应用。","ダウンロード完了！Wi-Fi終了後に適用。","Download fertig! Update nach WLAN-Exit.","Descarga completa. Se aplica al salir.","Téléchargement terminé. Appliqué à la sortie."],
msg_downloading:["Firmware is downloading. Device will reboot when done.","다운로드 중. 완료 후 재부팅됩니다.","固件下载中，完成后重启。","DL中。完了後に再起動。","Herunterladen... Neustart folgt.","Descargando... Se reiniciará.","Téléchargement en cours..."],
msg_confirm_upd:["Start download? The device may be unresponsive during download. The update will be applied on exit.","다운로드 시작? 다운로드 중 기기가 반응하지 않을 수 있습니다. Wi-Fi 모드 종료 시 적용됩니다.","开始下载？下载期间设备可能无响应，退出Wi-Fi后应用。","ダウンロード開始？ダウンロード中はデバイスが応答しない場合があります。Wi-Fi終了後に適用。","Herunterladen? Gerät reagiert möglicherweise nicht. Nach WLAN-Exit anwenden.","¿Iniciar descarga? El dispositivo puede no responder. Aplica al salir.","Lancer le téléchargement? L'appareil peut ne pas répondre. Appliqué à la sortie."],
msg_wifi_req:["Internet Wi-Fi required. Connect to Wi-Fi with internet access first.","인터넷 Wi-Fi 연결이 필요합니다. 먼저 인터넷이 연결된 Wi-Fi에 접속하세요.","需要连接互联网Wi-Fi，请先连接有互联网的Wi-Fi。","インターネットWi-Fiが必要です。先にインターネット接続のあるWi-Fiに接続してください。","WLAN mit Internet erforderlich. Bitte zuerst verbinden.","Se requiere Wi-Fi con internet. Conéctate primero.","Wi-Fi internet requis. Connectez-vous d'abord."],
msg_check_fail:["Check failed. Make sure Wi-Fi is connected to the internet.","확인 실패. 인터넷이 연결된 Wi-Fi에 접속되어 있는지 확인하세요.","检查失败，请确保Wi-Fi已连接至互联网。","確認失敗。インターネット接続を確認してください。","Fehler. Bitte WLAN mit Internet verbinden.","Error. Compruebe la conexión Wi-Fi a Internet.","Échec. Vérifiez la connexion Wi-Fi à Internet."],
msg_save_ok:["ID saved!","ID 저장 완료!","ID 已保存！","ID 保存完了！","ID gespeichert!","¡ID guardado!","ID enregistré !"],
msg_save_fail:["Failed to save ID.","ID 저장 실패.","ID 保存失败。","ID の保存に失敗。","ID konnte nicht gespeichert werden.","Error al guardar ID.","Impossible d'enregistrer l'ID."],
msg_test_fail:["Failed to start test.","테스트 시작 실패.","测试启动失败。","テスト開始に失敗。","Test konnte nicht gestartet werden.","Error al iniciar la prueba.","Impossible de démarrer le test."],
msg_status_fail:["Failed to load device status.","기기 상태 불러오기 실패.","加载设备状态失败。","デバイス状態の読み込みに失敗。","Gerätestatus konnte nicht geladen werden.","Error al cargar el estado del dispositivo.","Impossible de charger l'état de l'appareil."],
msg_conn_req_fail:["Failed to send connection request.","연결 요청 전송 실패.","发送连接请求失败。","接続リクエストの送信に失敗。","Verbindungsanfrage fehlgeschlagen.","Error al enviar solicitud de conexión.","Échec de la demande de connexion."],
        };
        function wt(k){const a=I18N[k];if(!a)return k;return a[window.LANG_IDX]||a[0]||k;}
        function applyI18n(){
            document.querySelectorAll('[data-i18n]').forEach(el=>{const t=wt(el.getAttribute('data-i18n'));if(t)el.textContent=t;});
            document.querySelectorAll('[data-i18n-html]').forEach(el=>{const t=wt(el.getAttribute('data-i18n-html'));if(t)el.innerHTML=t;});
            document.querySelectorAll('[data-i18n-ph]').forEach(el=>{const t=wt(el.getAttribute('data-i18n-ph'));if(t)el.placeholder=t;});
        }
        function escHtml(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
        function showMessage(text, type='info', duration=3000){let m=document.getElementById('global-message-box');if(!m){m=document.createElement('div');m.id='global-message-box';document.body.appendChild(m);}m.textContent=text;m.className='message-box message-'+type+' show';if(duration>0)setTimeout(()=>m.classList.remove('show'),duration);}
        document.addEventListener('DOMContentLoaded',function(){
            applyI18n();
            const cb=document.getElementById('lang-cur-btn');
            if(cb)cb.innerHTML='🌐 '+(_LC[window.LANG_IDX]||'EN')+' &#9662;';
            document.querySelectorAll('.lang-opt').forEach(b=>b.classList.toggle('active',+b.dataset.lang===window.LANG_IDX));
        });
    )rawliteral";

static void sendResponse(AsyncWebServerRequest *request, int code, const String& contentType, const String& content) {
    AsyncWebServerResponse *response = request->beginResponse(code, contentType, content);
    if (!response) {
        // 힙 부족 — 최소 응답으로 크래시 방지
        request->send(503, "text/plain", "Low memory");
        return;
    }
    response->addHeader("Connection", "close");
    request->send(response);
}

WebManager::WebManager() :
    _server(80), _ws("/ws"), _modeManager(nullptr), _commManager(nullptr),
    _isServerRunning(false), _otaUpdateDownloaded(false),
    _isScanningWifi(false), _isConnectingWifi(false), _isCheckingOta(false), _isDownloadingOta(false),
    _currentFirmwareVersion(FIRMWARE_VERSION), _latestOtaVersion("N/A"), _otaChangeLog("N/A"), _otaFirmwareUrl(""), _otaUpdateAvailable(false),
    _wifiEventId(0), _lastDisconnectReason(0), _wifiConnectStartMillis(0),
    _disconnectedForTestSsid(""), _reconnectOnExitTest(false),
    _pendingSaveCredential(false)
{
    _pendingCredSSID[0] = '\0';
    _pendingCredPwd[0]  = '\0';
    _otaDataMutex = xSemaphoreCreateMutex();
}

void WebManager::begin(ModeManager* modeMgr, CommManager* commMgr) {
    _instance = this;
    _modeManager = modeMgr;
    _commManager = commMgr;
    
    setupRoutes();
    setupWebSocket();
    setupLogBroadcaster();
    
    xTaskCreatePinnedToCore(
        [](void* param) { static_cast<WebManager*>(param)->loop(); },
        "WebManagerLoop", 4096, this, 1, NULL, 1
    );
    Log::Info(PSTR("WEB: WebManager initialized."));
}

void WebManager::loop() {
    for (;;) {
        if (_isServerRunning.load()) {
            _ws.cleanupClients();
            if (_pendingSaveCredential) {
                _pendingSaveCredential = false;
                Utils::saveWifiCredential(String(_pendingCredSSID), String(_pendingCredPwd));
                Log::Info(PSTR("WEB: Saved WiFi credentials for '%s'"), _pendingCredSSID);
            }
            if (_isConnectingWifi.load() && (millis() - _wifiConnectStartMillis > WIFI_CONNECT_TIMEOUT_MS)) {
                _isConnectingWifi = false;
                Log::Warn(PSTR("WEB: WiFi connection timed out."));
                WiFi.disconnect(true, true); 

                int reasonForBroadcast = (_lastDisconnectReason != 0) ? _lastDisconnectReason : 204;
                broadcastWifiStatus("failed", reasonForBroadcast);
                _lastDisconnectReason = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void WebManager::startServer() {
    if (_isServerRunning.load()) return;
    Log::Info(PSTR("WEB: Starting web server..."));
    uint8_t channel = _commManager->getChannel();
    // attemptAutoConnection()이 WiFi.begin()으로 STA 연결 중인 상태에서
    // softAPConfig()를 호출하면 DHCP 바인딩이 실패한다. WIFI_OFF 후
    // WIFI_AP_STA로 전환하면 AP가 완전히 클린 상태에서 시작된다.
    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(200));
    IPAddress apIP(AP_IP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, channel, 0, 4);
    if (!apStarted) {
        Log::Error(PSTR("WEB: softAP failed! AP will not be available."));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    _wifiEventId = WiFi.onEvent(WebManager::onWiFiEvent);
    _server.begin();
    _isServerRunning = true;
    _otaUpdateDownloaded = false;
    if (_modeManager) _modeManager->setUpdateDownloaded(false);
    Log::Info(PSTR("WEB: Server started. AP SSID: %s, Channel: %d"), AP_SSID, channel);
    Log::Info(PSTR("WEB: Access Point IP: http://%s"), WiFi.softAPIP().toString().c_str());
}

void WebManager::stopServer() {
    if (!_isServerRunning.load()) return;
    Log::Info(PSTR("WEB: Stopping web server..."));

    // [수정] 서버 객체를 정리하기 전에 루프가 더 이상 실행되지 않도록 플래그를 먼저 설정합니다.
    _isServerRunning = false;

    // 잠시 대기하여 WebManagerLoop가 플래그 변경을 인지할 시간을 줍니다.
    vTaskDelay(pdMS_TO_TICKS(150));

    _ws.closeAll();
    _server.end();

    // 등록했던 WiFi 이벤트 핸들러를 안전하게 제거합니다.
    if (_wifiEventId != 0) {
        WiFi.removeEvent(_wifiEventId);
        _wifiEventId = 0;
    }

    Log::Info(PSTR("WEB: Server stopped."));
}

void WebManager::reconnectWifiIfNeeded() {
    if (_reconnectOnExitTest && !_disconnectedForTestSsid.isEmpty()) {
        Log::Info(PSTR("WEB: Attempting to reconnect to Wi-Fi (%s) after exiting test mode."), _disconnectedForTestSsid.c_str());
        String pass = Utils::loadWifiPassword(_disconnectedForTestSsid);

        if (!pass.isEmpty()) {
            WiFi.begin(_disconnectedForTestSsid.c_str(), pass.c_str());
        } else {
            Log::Warn(PSTR("WEB: Could not find Wi-Fi password for reconnection (%s)."), _disconnectedForTestSsid.c_str());
        }
        _disconnectedForTestSsid = "";
        _reconnectOnExitTest = false;
    }
}

bool WebManager::isServerRunning() const { return _isServerRunning.load(); }

void WebManager::performUpdateAndReboot() {
    if (_otaUpdateDownloaded.load()) {
        Log::Info(PSTR("WEB: Applying OTA update and rebooting..."));
        // USB CDC를 명시적으로 분리해야 소프트 리셋 후 재열거가 정상 동작함
        Serial.end();
        delay(200);
        ESP.restart();
    }
}

void WebManager::broadcastTestComplete() {
    JsonDocument doc;
    doc["type"] = "test_completed";
    broadcastJson(doc);
}

// --- Page and API Handler Implementations ---

void WebManager::setupRoutes() {
        _server.on("/i18n.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "application/javascript", (const uint8_t*)I18N_JS, strlen_P(I18N_JS));
        response->addHeader("Connection", "close");
        request->send(response);
    });

    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r){ handleRoot(r); });
    _server.on("/manual", HTTP_GET, [this](AsyncWebServerRequest* r){ handleManualPage(r); });
    _server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest* r){ handleWifiConfigPage(r); });
    _server.on("/update", HTTP_GET, [this](AsyncWebServerRequest* r){ handleFirmwareUpdatePage(r); });
    _server.on("/test", HTTP_GET, [this](AsyncWebServerRequest* r){ handleTestModePage(r); });
    _server.on("/exit", HTTP_GET, [this](AsyncWebServerRequest* r){ handleExit(r); });
    
    _server.on("/api/scan-wifi", HTTP_GET, [this](AsyncWebServerRequest* r){ handleScanWifiApi(r); });
    _server.on("/api/connect-wifi", HTTP_POST, [this](AsyncWebServerRequest* r){ handleConnectWifiApi(r); });
    _server.on("/api/disconnect-wifi", HTTP_POST, [this](AsyncWebServerRequest* r){ handleDisconnectWifiApi(r); });
    _server.on("/api/wifi-status", HTTP_GET, [this](AsyncWebServerRequest* r){ handleWifiStatusApi(r); });
    _server.on("/api/check-ota", HTTP_GET, [this](AsyncWebServerRequest* r){ handleCheckOtaApi(r); });
    _server.on("/api/download-ota", HTTP_POST, [this](AsyncWebServerRequest* r){ handleDownloadOtaApi(r); });
    _server.on("/api/device-status", HTTP_GET, [this](AsyncWebServerRequest* r){ handleDeviceStatusApi(r); });
    _server.on("/api/set-device-id", HTTP_POST, [this](AsyncWebServerRequest* r){ handleSetDeviceIdApi(r); });
    _server.on("/api/run-test", HTTP_POST, [this](AsyncWebServerRequest* r){ handleRunTestApi(r); });

    _server.onNotFound([this](AsyncWebServerRequest* r){ handleNotFound(r); });
}

void WebManager::setupWebSocket() {
    _ws.onEvent([this](auto *s, auto *c, AwsEventType t, void *a, uint8_t *d, size_t l) { 
        onWsEvent(s, c, t, a, d, l); 
    });
    _server.addHandler(&_ws);
}

void WebManager::handleRoot(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->recordWebApiActivity();
    
    // [FIX] 테스트 모드에서 복귀 시 WiFi 재연결을 위해 이 라인을 다시 추가합니다.
    reconnectWifiIfNeeded();
    
    String html = getPageHeader("Nexus Flux Case", "pg_home");
    html += F("<div class='card'><h3 data-i18n='h_wifi_status'>Wi-Fi Status</h3><p id='home-wifi-status' data-i18n='w_loading'>Loading...</p></div>");
    html += F("<div class='card'><h3 data-i18n='h_dev_ctrl'>Device Control</h3>"
              "<p><a href='/wifi' class='btn' data-i18n='h_btn_wifi'>Wi-Fi Settings</a></p>"
              "<p><a href='/update' class='btn' data-i18n='h_btn_update'>Firmware Update</a></p>"
              "<p><a href='/test' class='btn' data-i18n='h_btn_test'>Test Mode</a></p>"
              "<p><a href='/manual' class='btn' data-i18n='h_btn_manual'>User Manual</a></p>"
              "<p><a href='/exit' class='btn btn-danger' data-i18n='h_btn_exit'>Exit Wi-Fi Mode</a></p>"
              "</div>");
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
                            if (s) {
                                if (d.connected) {
                                    s.innerHTML = wt('h_connected') + ": <b>" + escHtml(d.ssid) + "</b><br>" + wt('h_ip_addr') + " " + d.ip;
                                } else {
                                    s.textContent = wt('h_not_conn');
                                }
                            }
                        }
                    } catch(err) {
                        console.error(err);
                    }
                };
                ws.onclose = () => { setTimeout(connectWs, 2000); };
                ws.onerror = () => { ws.close(); };
            }
            document.addEventListener('DOMContentLoaded', connectWs);
        </script>
    )rawliteral";

html += getPageFooter(false);

    sendResponse(request, 200, "text/html; charset=UTF-8", html);
}

void WebManager::handleManualPage(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->recordWebApiActivity();
    
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
        .st{width:100%;border-collapse:collapse;font-size:13px;margin-top:8px;}
        .st th{background:rgba(109,40,217,0.3);color:#c4b5fd;padding:7px 8px;text-align:left;font-weight:600;}
        .st td{padding:6px 8px;border-top:1px solid rgba(255,255,255,0.05);color:#d1d5db;word-break:break-word;overflow-wrap:break-word;}
        .st tr:nth-child(even) td{background:rgba(255,255,255,0.02);}
        .st td:first-child,.st th:first-child{white-space:nowrap;}
    </style>
    <div class='card' style='text-align:left;'>
    <details open>
      <summary>💡 LED 상태 표시</summary>
      <div class='mb'>
        <p>기기 상단의 LED 색상과 패턴으로 현재 상태를 알 수 있습니다.</p>
        <table class='st'>
          <tr><th>LED 패턴</th><th>의미</th></tr>
          <tr><td>꺼짐</td><td>정상 대기 중 — 리모컨 신호를 기다리고 있습니다</td></tr>
          <tr><td>켜짐 (지속)</td><td>출력 중 — 전자석(MOSFET) 및 부스트가 활성화되어 전원이 인가되고 있습니다</td></tr>
          <tr><td>1초 켜짐 후 꺼짐</td><td>부팅 완료 또는 페어링 성공</td></tr>
          <tr><td>N회 깜빡임</td><td>현재 기기 ID를 숫자로 표시 (예: 3번 깜빡임 = ID 3)</td></tr>
          <tr><td>빠른 반복 깜빡임</td><td>페어링 대기 중 — 리모컨의 MAC 주소를 기다립니다 (30초 타임아웃)</td></tr>
          <tr><td>3회 깜빡임</td><td>Wi-Fi 모드 전환 중</td></tr>
        </table>
        <div class='tb'>💡 ID 설정 중에는 ID 버튼을 누를 때마다 짧게 한 번 깜빡여 번호 입력을 확인합니다.</div>
      </div>
    </details>
    <details>
      <summary>🔢 기기 ID 설정</summary>
      <div class='mb'>
        <p>각 기기는 고유한 ID(1~20)를 가집니다. 리모컨은 이 ID로 원하는 기기를 지정합니다.</p>
        <p style='margin-bottom:4px;'><b>현재 ID 확인:</b></p>
        <ol class='sl'>
          <li>ID 버튼을 <b>짧게</b> 누르면 LED가 현재 ID 번호만큼 깜빡입니다</li>
        </ol>
        <p style='margin-top:12px;margin-bottom:4px;'><b>ID 변경:</b></p>
        <ol class='sl'>
          <li>ID 버튼을 <b>길게</b> (2초) → ID 설정 모드 진입 (LED 1초 켜짐)</li>
          <li>ID 버튼을 <b>짧게</b> 눌러 원하는 번호로 이동 (1→2→...→20→1 순환, 누를 때마다 깜빡임)</li>
          <li>ID 버튼을 <b>길게</b> → 번호 확정 (LED 1초 켜짐 후 번호만큼 깜빡임)</li>
        </ol>
        <div class='tb'>💡 ID 설정 모드에서 5초 동안 아무 버튼도 누르지 않으면 이전 ID로 자동 복원됩니다.</div>
        <div class='tb'>💡 ID 설정 모드에서 EXEC 버튼을 누르면 페어링 대기로 바로 진입합니다.</div>
        <div class='wb'>⚠ 같은 그룹에 속한 기기끼리는 서로 다른 ID를 사용해야 합니다.</div>
      </div>
    </details>
    <details>
      <summary>⚡ EXEC 버튼 수동 작동</summary>
      <div class='mb'>
        <p>리모컨 없이 기기 본체의 EXEC 버튼으로 직접 출력을 활성화할 수 있습니다.</p>
        <p style='margin-top:10px;margin-bottom:4px;'><b>대기 중 (정상 상태):</b></p>
        <p style='margin:0 0 0 8px;'>누르는 동안 100% 출력 (전자석 잠금 해제/잠금 작동). 손을 떼면 즉시 정지</p>
        <p style='margin-top:8px;margin-bottom:4px;'><b>리모컨 시퀀스 실행 중:</b></p>
        <p style='margin:0 0 0 8px;'>누르면 현재 실행 중인 시퀀스를 강제 중단합니다</p>
        <div class='wb' style='margin-top:10px;'>⚠ 안전을 위해 EXEC 버튼은 500ms 쿨다운이 있습니다.</div>
      </div>
    </details>
    <details>
      <summary>🛑 리모컨 취소 명령</summary>
      <div class='mb'>
        <p>리모컨(송신기)의 <span class='kb'>B</span> 버튼으로 실행 중인 시퀀스를 원격으로 제어할 수 있습니다.</p>
        <table class='st'>
          <tr><th>동작</th><th>결과</th></tr>
          <tr><td><span class='kb'>▶</span> (플레이) 짧게 누름</td><td>리모컨만 홈 화면으로 복귀. <b>수신기 출력은 계속 진행</b>됩니다.</td></tr>
          <tr><td><span class='kb'>B</span> 0.5초 이상 누름</td><td>모든 수신기에 즉시 취소 신호 전송 → 잠금 해제/출력 즉시 중단.</td></tr>
        </table>
        <div class='wb' style='margin-top:10px;'>⚠ 취소 신호는 ESP-NOW 브로드캐스트로 전송되므로, 리모컨과 수신기가 같은 채널에 있어야 작동합니다.</div>
      </div>
    </details>
    <details>
      <summary>🔗 리모컨 페어링</summary>
      <div class='mb'>
        <p>이 기기에 리모컨의 마스터 MAC 주소를 등록하는 과정입니다. 한 번만 하면 됩니다.</p>
        <ol class='sl'>
          <li>ID 버튼을 <b>길게</b> 눌러 ID 설정 모드 진입</li>
          <li>ID 변경 필요 시: ID 버튼 <b>짧게</b> 눌러 번호 이동 → <b>길게</b> 눌러 확정</li>
          <li>EXEC 버튼 → 페어링 대기 모드 진입 (LED 빠른 깜빡임)</li>
          <li>리모컨에서: 홈 메뉴 → <b>페어링</b> → <span class='kb'>▶</span></li>
          <li>수신기 LED가 꺼지면 페어링 완료!</li>
        </ol>
        <div class='tb'>✅ 페어링 완료 후 기기는 등록된 리모컨의 신호에만 반응합니다.</div>
        <div class='wb'>⚠ 페어링 대기는 30초 타임아웃입니다. 시간 내에 리모컨에서 페어링 신호를 보내세요.</div>
      </div>
    </details>
    <details>
      <summary>📡 Wi-Fi 모드 진입·종료</summary>
      <div class='mb'>
        <p>Wi-Fi 모드에서 기기 설정, 펌웨어 업데이트, 테스트 등을 할 수 있습니다.</p>
        <p style='margin-bottom:4px;'><b>진입 방법:</b></p>
        <ol class='sl'>
          <li>ID 버튼과 EXEC 버튼을 <b>동시에 길게</b> (2초) 누르세요</li>
          <li>LED 3회 깜빡임 후 Wi-Fi 모드 활성화</li>
          <li>스마트폰 Wi-Fi에서 <b>Nexus_Flux_Case</b>에 연결</li>
          <li>브라우저에서 <b>192.168.4.1</b> 접속</li>
        </ol>
        <p style='margin-top:12px;margin-bottom:4px;'><b>종료 방법 (아래 중 하나):</b></p>
        <ol class='sl'>
          <li>웹 페이지 하단의 <b>Wi-Fi 모드 종료</b> 버튼 클릭</li>
          <li>ID + EXEC 동시에 다시 길게 누르기</li>
          <li>5분 동안 웹 활동 없으면 자동 종료</li>
        </ol>
        <div class='wb'>⚠ Wi-Fi 모드에서는 리모컨 신호를 받지 않습니다.</div>
      </div>
    </details>
    <details>
      <summary>🧪 테스트 모드</summary>
      <div class='mb'>
        <p>테스트 모드 페이지에서 기기 출력을 직접 테스트해 볼 수 있습니다.</p>
        <table class='st'>
          <tr><th>기능</th><th>설명</th></tr>
          <tr><td>기기 ID</td><td>기기 ID를 웹에서 직접 변경·저장</td></tr>
          <tr><td>딜레이 (초)</td><td>테스트 시 출력 시작 전 대기 시간</td></tr>
          <tr><td>실행 시간 (초)</td><td>테스트 시 출력 지속 시간</td></tr>
          <tr><td>수동 테스트 실행</td><td>설정한 딜레이·실행 시간으로 즉시 테스트 실행</td></tr>
        </table>
      </div>
    </details>
    <details>
      <summary>🔄 펌웨어 업데이트</summary>
      <div class='mb'>
        <p>인터넷이 되는 Wi-Fi에 연결한 후 펌웨어를 무선으로 무선 다운로드받아 업데이트할 수 있습니다.</p>
      </div>
    </details>
    <details>
      <summary>💡 마술 연출 및 응용 가이드</summary>
      <div class='mb'>
        <p><b>Nexus Flux Case</b>(Magnet) 수신기는 <b>디지털 ON/OFF 출력</b> 제어 및 고전압 부스트 회로를 내장하여 전자석 잠금 장치 구동에 최적화되어 있습니다.</p>
        <p style='margin-bottom:4px;'><b>대표적인 마술 연출 및 응용 예시:</b></p>
        <ol class='sl'>
          <li><b>카드 케이스 잠금 해제 (Flux Card Case)</b>: 리모컨 신호를 통해 잠겨있던 마그네틱 카드 케이스를 원격으로 순간 잠금 해제하여 관객이 카드를 확인할 수 있게 하거나, 비밀 서랍을 자동으로 개방합니다.</li>
          <li><b>밀폐 잠금 상자 (Locked Puzzle Box)</b>: 상자 내부에 부착된 전자석 잠금 장치를 제어하여 마술사의 신호에 따라 상자가 잠기거나 부드럽게 열리도록 연출합니다.</li>
          <li><b>정신력 연출 (Telekinesis)</b>: 관객이 뚜껑을 열려고 할 때 원격으로 잠그고, 마술사가 신호를 주면 부드럽게 열리게 하는 초능력 연출을 구현합니다.</li>
        </ol>
        <div class='tb'>💡 <b>부스트 기능 및 송신기 설정</b>: 출력 시 부스트 회로(BOOST_EN_PIN)를 함께 기동하여 코일에 높은 기동 전압을 공급하므로 흡착 및 걸쇠 해제 기능의 신뢰도가 매우 높습니다. 100% 디지털 출력이므로 송신기에서 <b>POWER(출력 세기)를 100%</b>로 설정하고 조작해 주십시오.</div>
        <div class='wb'>⚠ <b>배터리 및 발열 관리</b>: 고출력 전자석을 장시간 켜두면 전자석 코일이 매우 가열되고 배터리 전력 소모가 극심해집니다. 따라서 실행 시간(PLAY)은 <b>1.5초~3초</b> 이내의 최소한의 짧은 잠금 해제 시간으로 설정하여 기기 수명과 배터리를 보존하십시오.</div>
      </div>
    </details>
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
}

void WebManager::handleWifiConfigPage(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->recordWebApiActivity();
    String html = getPageHeader("Wi-Fi Settings", "pg_wifi");
    html += R"rawliteral(
        <div class="card">
            <h2 data-i18n='w_cur_status'>Current Wi-Fi Status</h2>
            <p id="conn-status" data-i18n='w_loading'>Loading...</p>
        </div>
        <div class="card">
            <h2 data-i18n='w_conn_card'>Wi-Fi Connection</h2>
            <div class="form-group" style="text-align: center;">
                <label for="ssid-select" data-i18n='w_sel_ssid'>Select SSID:</label>
                <select id="ssid-select" class="form-control">
                    <option value="" data-i18n-ph='w_scan_ph'>-- Scan to select a network --</option>
                </select>
            </div>
            <div style="display: flex; justify-content: center; margin-top: 20px; gap: 10px;">
                <button id="scan-btn" class="btn" onclick="scanWifi()" data-i18n='w_rescan'>Rescan</button>
            </div>
            <div class="form-group" style="text-align: center;">
                <label for="password-input" data-i18n='w_pass'>Password:</label>
                <input type="password" id="password-input" class="form-control">
            </div>
            <button id="action-btn" class="btn" onclick="handleConnectDisconnect()" data-i18n='w_connect'>Connect</button>
        </div>
        <script>
            const scanBtn = document.getElementById("scan-btn");
            const actionBtn = document.getElementById("action-btn");
            const ssidSelect = document.getElementById("ssid-select");
            const passwordInput = document.getElementById("password-input");
            const connStatusEl = document.getElementById("conn-status");
            let ws, currentSsid = '', isConnected = false;

            function connectWs() {
                ws = new WebSocket('ws://'+location.host+'/ws');
                ws.onopen = () => { fetchStatus(); };
                ws.onclose = () => { setTimeout(connectWs, 2000); };
                ws.onmessage = evt => {
                    try {
                        const data = JSON.parse(evt.data);
                        if (data.type === "wifi_status_update") handleWifiStatus(data);
                        if (data.type === "scan_result") handleScanResult(data);
                    } catch (e) {}
                };
            }

            function fetchStatus() {
                fetch("/api/wifi-status").then(r=>r.json()).then(data=>handleWifiStatus(data)).catch(()=>{});
            }

            function handleConnectDisconnect() {
                if (!isConnected) connectWifi();
                else disconnectWifi();
            }

            function handleWifiStatus(data) {
                currentSsid = data.connected ? data.ssid : '';
                isConnected = !!data.connected;
                actionBtn.disabled = false; scanBtn.disabled = false;
                passwordInput.disabled = false; ssidSelect.disabled = false;
                actionBtn.classList.remove('btn-danger','btn-success');
                if (data.connected) {
                    connStatusEl.innerHTML = wt('h_connected')+' <b>'+escHtml(data.ssid)+'</b> ('+wt('h_ip_addr')+' '+escHtml(data.ip)+')';
                    actionBtn.textContent = wt('w_disconnect');
                    actionBtn.classList.add('btn-danger');
                    passwordInput.disabled = true; ssidSelect.disabled = true; scanBtn.disabled = true;
                    if (data.status === "connected") showMessage(wt('msg_conn_ok'), 'success');
                } else {
                    connStatusEl.textContent = wt('msg_not_conn_s');
                    actionBtn.textContent = wt('w_connect');
                    switch (data.status) {
                        case "connecting":
                            showMessage(wt('msg_connecting'), 'info', 0);
                            actionBtn.disabled = true; scanBtn.disabled = true;
                            passwordInput.disabled = true; ssidSelect.disabled = true;
                            break;
                        case "failed":
                            if (data.reason === 15 || data.reason === 2 || data.reason === 8) showMessage(wt('msg_conn_fail'), 'error');
                            else if (data.reason === 201) showMessage(wt('msg_conn_fail_range'), 'error');
                            else showMessage(wt('msg_conn_fail_env'), 'error');
                            break;
                        case "disconnected": showMessage(wt('msg_disconnected'), 'info'); break;
                        default: break;
                    }
                }
            }

            function scanWifi() {
                if (scanBtn.disabled) return;
                scanBtn.disabled = true;
                ssidSelect.innerHTML = "<option>"+wt('msg_scanning_opt')+"</option>";
                showMessage(wt('msg_scanning'), "info");
                fetch("/api/scan-wifi").catch(() => { showMessage(wt('msg_scan_fail'), "error"); scanBtn.disabled = false; });
            }

            function handleScanResult(data) {
                ssidSelect.innerHTML = "<option value=''>"+wt('msg_sel_net_ph')+"</option>";
                if (data.networks && data.networks.length > 0) {
                    data.networks.slice(0, 20).forEach(net => {
                        ssidSelect.add(new Option((net.encrypted?"🔒 ":"  ")+net.ssid+" ("+net.rssi+" dBm)", net.ssid));
                    });
                    showMessage(wt('msg_scan_ok'), "success");
                } else {
                    ssidSelect.innerHTML = "<option>"+wt('msg_no_nets')+"</option>";
                    showMessage(wt('msg_no_nets'), "error");
                }
                scanBtn.disabled = false;
            }

            function connectWifi() {
                const ssid = ssidSelect.value;
                if (!ssid) { showMessage(wt('msg_select_net'), "error"); return; }
                actionBtn.disabled = true; scanBtn.disabled = true;
                passwordInput.disabled = true; ssidSelect.disabled = true;
                showMessage(wt('msg_connecting'), "info", 0);
                fetch("/api/connect-wifi", {
                    method: "POST",
                    headers: { "Content-Type": "application/x-www-form-urlencoded" },
                    body: "ssid="+encodeURIComponent(ssid)+"&password="+encodeURIComponent(passwordInput.value)
                }).catch(() => {
                    showMessage(wt('msg_conn_req_fail'), "error");
                    actionBtn.disabled = false; scanBtn.disabled = false;
                    passwordInput.disabled = false; ssidSelect.disabled = false;
                });
            }

            function disconnectWifi() {
                if (!window.confirm(wt('msg_confirm_disc'))) return;
                actionBtn.disabled = true;
                showMessage(wt('msg_disconnecting'), 'info', 0);
                fetch("/api/disconnect-wifi", {
                    method: "POST",
                    headers: { "Content-Type": "application/x-www-form-urlencoded" },
                    body: "ssid="+encodeURIComponent(currentSsid)
                }).then(() => { passwordInput.value = ''; })
                  .catch(() => { showMessage(wt('msg_disc_fail'), 'error'); });
            }

            window.onload = () => { connectWs(); fetchStatus(); setTimeout(scanWifi, 500); };
        </script>
    )rawliteral";
    html += getPageFooter(true);
    sendResponse(request, 200, "text/html; charset=UTF-8", html);
}

void WebManager::handleFirmwareUpdatePage(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->recordWebApiActivity();
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
                    <div class='progress-bar'><div class='progress-bar-inner' id='progress-bar-inner'></div></div>
                </div>
                <p id='download-notice' class='hidden notice' data-i18n='msg_downloading'>Firmware is downloading. Device will reboot when done.</p>
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
                    } catch(err) {}
                };
                ws.onopen = () => fetch("/api/check-ota");
                ws.onclose = () => setTimeout(connectWs, 2000);
            }

            function updateOtaUi(d) {
                document.getElementById("current-v").textContent = d.current_version;
                document.getElementById("latest-v").textContent = d.latest_version;
                if (!d.internet_ok) {
                    updateStatus.innerHTML = "<span style='color:#f87171;'>"+wt('msg_wifi_req')+"</span>";
                    changelogEl.textContent = wt('msg_check_fail');
                    updateBtn.classList.add("hidden");
                    downloadProgressDiv.classList.add('hidden');
                    return;
                }
                changelogEl.textContent = d.check_ok ? (d.changelog || '') : wt('msg_check_fail');
                if (d.update_available) {
                    updateStatus.innerHTML = "<b style='color:#34d399;'>"+wt('msg_update_avail')+"</b>";
                    updateBtn.classList.remove("hidden");
                    updateBtn.disabled = false;
                    updateBtn.textContent = wt('u_btn');
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
                downloadNotice.textContent = wt('msg_downloading');
                downloadNotice.classList.remove("hidden");
                downloadProgressDiv.classList.remove('hidden');
                updateProgressText(0);
                fetch("/api/download-ota", { method: "POST" });
            }

            function updateProgressText(progress) {
                progressText.textContent = progress+"%";
                progressBarInner.style.width = progress+"%";
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
}

void WebManager::handleTestModePage(AsyncWebServerRequest* request) {
    if (_modeManager) _modeManager->switchToMode(DeviceMode::MODE_TEST);

    if (WiFi.status() == WL_CONNECTED) {
        Log::Info(PSTR("WEB: Entering Test Mode. Temporarily disconnecting Wi-Fi."));
        _disconnectedForTestSsid = WiFi.SSID();
        _reconnectOnExitTest = true;
        WiFi.disconnect(true);
    }

    String html = getPageHeader("Test Mode", "pg_test");
    html += R"rawliteral(
        <div class='card'>
            <h3 data-i18n='dev_settings'>Device Settings</h3>
            <table style='width:100%; text-align:left; border-spacing: 0 10px; border-collapse: separate;'>
              <tr>
                <td style='width:140px;'><label for='dev-id' data-i18n='dev_id_label'>Device ID :</label></td>
                <td>
                  <div style='display:flex; align-items:center;'>
                    <input type='number' id='dev-id' min='1' max='20' style='width: 80px; margin:0;'>
                    <button onclick='saveId()' class='btn' data-i18n='save_btn' style='padding:5px 10px; min-width:auto; margin-left: 10px;'>Save</button>
                  </div>
                </td>
              </tr>
              <tr>
                <td><label for='delay-s' data-i18n='delay_label'>Delay Timer (s) :</label></td>
                <td><input type='number' id='delay-s' step='0.1' style='width: 80px;'></td>
              </tr>
              <tr>
                <td><label for='play-s' data-i18n='play_label'>Play Timer (s) :</label></td>
                <td><input type='number' id='play-s' step='0.1' style='width: 80px;'></td>
              </tr>
            </table>
            <p><button onclick='runTest()' id='run-test-btn' class='btn' data-i18n='run_btn'>Run Manual Test</button></p>
        </div>
        <div class='card'>
            <h3><span data-i18n='log_title'>Live Log</span> (<a href='javascript:void(0);' onclick='document.getElementById("log").innerHTML=""' data-i18n='log_clear'>Clear</a>)</h3>
            <div id='log' style='height:300px;overflow-y:scroll;border:1px solid rgba(255,255,255,0.12);text-align:left;padding:5px;font-family:monospace;font-size:0.9em;background:rgba(0,0,0,0.4);color:#e5e7eb;white-space:pre-wrap;border-radius:8px;'></div>
            <p style='margin-top:15px; font-weight: bold; color: #f87171;' data-i18n-html='no_remote_warn'>
                This mode does not support connection with the transmitter.<br>Communication will be enabled when you exit this mode.
            </p>
        </div>
        <script>
            let log=document.getElementById("log");
            let ws;

            function getStatus(){
                fetch("/api/device-status")
                .then(r=>r.json())
                .then(d=>{
                    document.getElementById("dev-id").value = d.device_id;
                    document.getElementById("delay-s").value = d.test_delay_ms / 1000.0;
                    document.getElementById("play-s").value = d.test_play_ms / 1000.0;
                })
                .catch(() => showMessage(wt('msg_status_fail'), 'error'));
            }

            function saveId(){
                fetch("/api/set-device-id",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"id="+document.getElementById("dev-id").value})
                .then(response => {
                    if (response.ok) showMessage(wt('msg_save_ok'), 'success');
                    else showMessage(wt('msg_save_fail'), 'error');
                })
                .catch(() => showMessage(wt('msg_save_fail'), 'error'));
            }

            function runTest(){
                let btn = document.getElementById('run-test-btn');
                btn.disabled = true;
                btn.textContent = wt('running_btn');
                let delayMs = parseFloat(document.getElementById('delay-s').value) * 1000;
                let playMs = parseFloat(document.getElementById('play-s').value) * 1000;
                let formData = new URLSearchParams();
                formData.append('delay', delayMs);
                formData.append('play', playMs);
                fetch("/api/run-test",{
                    method:"POST",
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: formData
                }).catch(() => {
                    showMessage(wt('msg_test_fail'), 'error');
                    btn.disabled = false;
                    btn.textContent = wt('run_btn');
                });
            }

            function connectWs() {
                ws = new WebSocket("ws://"+window.location.host+"/ws");
                ws.onmessage=e=>{
                    try{
                        let d=JSON.parse(e.data);
                        if(d.type==="log"){
                            log.innerHTML+='<div style="color:#fff;">['+( d.ts / 1000).toFixed(1)+'s] '+d.msg+'</div>';
                            log.scrollTop=log.scrollHeight;
                        }
                        if(d.type==="test_completed"){
                            let btn = document.getElementById('run-test-btn');
                            btn.disabled = false;
                            btn.textContent = wt('run_btn');
                            log.innerHTML+='<div style="color:#60a5fa;">['+( Date.now() / 1000).toFixed(1)+'s] Test Completed.</div>';
                        }
                    }catch(err){}
                };
                ws.onclose = () => { setTimeout(connectWs, 2000); };
                ws.onerror = () => { ws.close(); };
            }
            window.onload = () => { getStatus(); connectWs(); };
        </script>
    )rawliteral";
    html += getPageFooter(true);
    sendResponse(request, 200, "text/html; charset=UTF-8", html);
}

void WebManager::handleExit(AsyncWebServerRequest* request) {
    String html = getPageHeader("Exiting Wi-Fi Mode", "pg_exit");
    html += F("<p data-i18n='exit_msg'>The device will now return to normal operation. You can close this window.</p>");
    if (_otaUpdateDownloaded.load()) {
        html += F("<p style='color:#60a5fa;font-weight:bold;' data-i18n='exit_ota_msg'>An update was downloaded and will be applied on reboot.</p>");
    }
    html += getPageFooter(false);
    sendResponse(request, 200, "text/html; charset=UTF-8", html);
    vTaskDelay(pdMS_TO_TICKS(300));
    if (_modeManager) _modeManager->exitWifiMode();
}

void WebManager::handleNotFound(AsyncWebServerRequest* request) { 
    sendResponse(request, 404, "text/plain", "Not Found"); 
}

void WebManager::handleScanWifiApi(AsyncWebServerRequest* request) {
    if (_isScanningWifi.exchange(true)) {
        sendResponse(request, 429, "application/json", "{\"status\":\"busy\", \"message\":\"Scan already in progress.\"}");
        return;
    }
    BaseType_t res = xTaskCreate(wifiScanTask, "wifiScanTask", 4096, this, 5, NULL);
    if (res != pdPASS) {
        _isScanningWifi = false;
        sendResponse(request, 500, "application/json", "{\"error\":\"Memory allocation failed\"}");
        return;
    }
    sendResponse(request, 202, "application/json", "{\"status\":\"accepted\", \"message\":\"Scan started.\"}");
}

void WebManager::handleConnectWifiApi(AsyncWebServerRequest* request) {
    if (_isConnectingWifi.load()) {
        sendResponse(request, 429, "application/json", "{\"error\":\"Connection already in progress\"}");
        return;
    }
    if (!request->hasParam("ssid", true)) {
        sendResponse(request, 400, "application/json", "{\"error\":\"Missing SSID\"}");
        return;
    }

    String ssid = request->getParam("ssid", true)->value();
    String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";

    Log::Info(PSTR("WEB: Received connect request for SSID: %s"), ssid.c_str());

    if (_isConnectingWifi.exchange(true)) {
        sendResponse(request, 429, "application/json", "{\"error\":\"Connection already in progress\"}");
        return;
    }
    _wifiConnectStartMillis = millis();

    strncpy(_pendingCredSSID, ssid.c_str(), sizeof(_pendingCredSSID) - 1);
    _pendingCredSSID[sizeof(_pendingCredSSID) - 1] = '\0';
    strncpy(_pendingCredPwd, password.c_str(), sizeof(_pendingCredPwd) - 1);
    _pendingCredPwd[sizeof(_pendingCredPwd) - 1] = '\0';
    _pendingSaveCredential = false;

    broadcastWifiStatus("connecting");
    
    WiFi.disconnect(true, true);
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.begin(ssid.c_str(), password.c_str());

    sendResponse(request, 202, "application/json", "{\"status\":\"connection_attempt_started\"}");
}

void WebManager::handleWifiStatusApi(AsyncWebServerRequest* request) {
    JsonDocument doc;
    getWifiStatusJson(doc);
    if(request) {
        String output; 
        serializeJson(doc, output);
        sendResponse(request, 200, "application/json", output);
    }
}

void WebManager::handleCheckOtaApi(AsyncWebServerRequest* request) {
    if (_isCheckingOta.exchange(true)) {
        sendResponse(request, 200, "application/json", "{\"status\":\"already_checking\"}");
        return;
    }
    // Reduced stack from 8KB to 6KB to save RAM
    BaseType_t res = xTaskCreate(otaCheckVersionTask, "otaCheckTask", 6144, this, 5, NULL);
    if (res != pdPASS) {
        _isCheckingOta = false;
        sendResponse(request, 500, "application/json", "{\"error\":\"Memory allocation failed\"}");
        return;
    }
    sendResponse(request, 200, "application/json", "{\"status\":\"checking\"}");
}

void WebManager::handleDownloadOtaApi(AsyncWebServerRequest* request) {
    if (_isDownloadingOta.exchange(true)) {
        sendResponse(request, 429, "application/json", "{\"status\":\"ota_download_in_progress\"}");
        return;
    }
    // Reduced stack from 10KB to 8KB to save RAM
    BaseType_t res = xTaskCreate(otaDownloadTask, "otaDownloadTask", 8192, this, 2, NULL);
    if (res != pdPASS) {
        _isDownloadingOta = false;
        sendResponse(request, 500, "application/json", "{\"error\":\"Memory allocation failed\"}");
        return;
    }
    sendResponse(request, 200, "application/json", "{\"status\":\"download_started\"}");
}

void WebManager::handleDeviceStatusApi(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["device_id"] = Utils::loadDeviceId();
    doc["test_delay_ms"] = Utils::loadTestDelay();
    doc["test_play_ms"] = Utils::loadTestPlay();
    String output; serializeJson(doc, output);
    sendResponse(request, 200, "application/json", output);
}

void WebManager::handleSetDeviceIdApi(AsyncWebServerRequest* request) {
    if (!_modeManager || !request->hasParam("id", true)) {
        sendResponse(request, 400, "application/json", "{\"error\":\"bad request\"}");
        return;
    }
    int idVal = request->getParam("id", true)->value().toInt();
    if (idVal < MIN_DEVICE_ID || idVal > MAX_DEVICE_ID) {
        sendResponse(request, 400, "application/json", "{\"error\":\"id out of range\"}");
        return;
    }
    _modeManager->updateDeviceId((uint8_t)idVal, true);
    sendResponse(request, 200, "application/json", "{\"status\":\"ok\"}");
}

void WebManager::handleRunTestApi(AsyncWebServerRequest* request) {
    uint32_t delayMs = Utils::loadTestDelay(); // Get default delay
    uint32_t playMs = Utils::loadTestPlay();   // Get default play duration

    // Override with values from request if provided
    if (request->hasParam("delay", true)) {
        delayMs = request->getParam("delay", true)->value().toInt();
    }
    if (request->hasParam("play", true)) {
        playMs = request->getParam("play", true)->value().toInt();
    }

    if (_modeManager) {
        _modeManager->triggerManualRun(delayMs, playMs); // Trigger manual test run in ModeManager
    }
    sendResponse(request, 200, "application/json", "{\"status\":\"started\"}");
}

void WebManager::handleDisconnectWifiApi(AsyncWebServerRequest* request) {
    Log::Info(PSTR("WEB: Received disconnect request."));
    if (request->hasParam("ssid", true)) {
        String ssidToForget = request->getParam("ssid", true)->value();
        Utils::removeWifiCredential(ssidToForget); // Remove credential from NVS
        Log::Info(PSTR("WEB: Wi-Fi credential for %s was forgotten."), ssidToForget.c_str());
    }
    
    WiFi.disconnect(true, true); // Disconnect from WiFi and erase credentials
    sendResponse(request, 200, "application/json", "{\"status\":\"disconnected\"}");
}

// --- Event Handlers & Helpers ---

void WebManager::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (!_instance) return;
    
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            Serial.println("WiFi client started");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_STOP:
            Serial.println("WiFi client stopped");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("Connected to WiFi network");
            _instance->_lastDisconnectReason = 0;
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.printf("Disconnected from WiFi network. Reason: %d\n", info.wifi_sta_disconnected.reason);
            _instance->_lastDisconnectReason = info.wifi_sta_disconnected.reason;

            if (_instance->_isConnectingWifi.load()) {
                int reason = info.wifi_sta_disconnected.reason;
                if (reason == 15 || reason == 201 || reason == 2 || reason == 8) {
                    Log::Warn(PSTR("WEB: WiFi connection failed with definitive reason: %d. Broadcasting failure."), reason);
                    _instance->_isConnectingWifi = false;
                    _instance->broadcastWifiStatus("failed", reason);
                } else {
                    Log::Debug(PSTR("WEB: Transient disconnect during connection attempt (Reason: %d). Waiting for final status."), reason);
                }
            } else {
                _instance->broadcastWifiStatus("disconnected", info.wifi_sta_disconnected.reason);
            }
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("Got IP address: %s\n", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
            if (_instance) {
                if (_instance->_isConnectingWifi.load()) {
                    _instance->_isConnectingWifi = false;
                    _instance->_pendingSaveCredential = true;
                }
                _instance->_lastDisconnectReason = 0;
                _instance->broadcastWifiStatus("connected");
            }
            break;
            
        case ARDUINO_EVENT_WIFI_AP_START:
            Serial.println("AP started");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STOP:
            Serial.println("AP stopped");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("Client connected to AP");
            break;
            
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("Client disconnected from AP");
            break;
        default:
            break;
    }
}

void WebManager::onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (_modeManager) _modeManager->recordWebApiActivity();
    if (type == WS_EVT_CONNECT) {
        Log::Debug(PSTR("WEB: WebSocket client #%u connected from %s"), client->id(), client->remoteIP().toString().c_str());
        
        JsonDocument doc;
        getWifiStatusJson(doc);
        doc["type"] = "wifi_status_update";
        
        String output;
        serializeJson(doc, output);
        client->text(output);
        
        broadcastOtaStatus();
    } else if (type == WS_EVT_DISCONNECT) {
        Log::Debug(PSTR("WEB: WebSocket client #%u disconnected."), client->id());
    }
}

void WebManager::wifiScanTask(void* pvParameters) {
    WebManager* self = static_cast<WebManager*>(pvParameters);
    Log::Info(PSTR("WEB: Starting WiFi scan..."));

    int n = WiFi.scanNetworks();
    
    JsonDocument doc;
    doc["type"] = "scan_result";
    JsonArray networksArray = doc["networks"].to<JsonArray>();

    if (n > 0) {
        Log::Info(PSTR("WEB: Scan found %d networks."), n);
        std::vector<std::tuple<String, int, wifi_auth_mode_t>> found_networks;
        for (int i = 0; i < n; ++i) {
            found_networks.push_back(std::make_tuple(
                WiFi.SSID(i),
                WiFi.RSSI(i),
                WiFi.encryptionType(i)
            ));
        }
        
        std::sort(found_networks.begin(), found_networks.end(),
            [](const auto& a, const auto& b) {
                return std::get<1>(a) > std::get<1>(b);
            });

        for (const auto& net : found_networks) {
            JsonObject netObj = networksArray.add<JsonObject>();
            netObj["ssid"] = std::get<0>(net);
            netObj["rssi"] = std::get<1>(net);
            netObj["encrypted"] = (std::get<2>(net) != WIFI_AUTH_OPEN);
        }
    } else {
        Log::Warn(PSTR("WEB: Scan failed or no networks found. Result: %d"), n);
    }

    self->broadcastJson(doc);
    WiFi.scanDelete();
    
    self->_isScanningWifi = false;
    vTaskDelete(NULL);
}

void WebManager::getWifiStatusJson(JsonDocument& doc) {
    doc["connected"] = WiFi.status() == WL_CONNECTED;
    if (doc["connected"]) {
        doc["ssid"] = WiFi.SSID();
        doc["rssi"] = WiFi.RSSI();
        doc["ip"] = WiFi.localIP().toString();
    }
    doc["scanning"] = _isScanningWifi.load();
    doc["connecting"] = _isConnectingWifi.load();
    if (_lastDisconnectReason != 0) {
        doc["last_disconnect_reason"] = _lastDisconnectReason;
    }
}

void WebManager::broadcastWifiStatus(const char* status, int reason) {
    JsonDocument doc;
    getWifiStatusJson(doc);
    
    doc["type"] = "wifi_status_update";
    doc["status"] = status;
    if (reason != 0) {
        doc["reason"] = reason;
    }
    broadcastJson(doc);
}

void WebManager::broadcastOtaStatus() {
    JsonDocument doc;
    bool isConnected = (WiFi.status() == WL_CONNECTED);

    doc["type"] = "ota_status";
    
    bool checkOk = isConnected && !_latestOtaVersion.isEmpty();
    doc["internet_ok"] = isConnected;
    doc["check_ok"] = checkOk;
    doc["current_version"] = _currentFirmwareVersion;
    doc["latest_version"] = isConnected ? _latestOtaVersion : "N/A";
    doc["update_available"] = isConnected ? _otaUpdateAvailable : false;
    doc["changelog"] = checkOk ? _otaChangeLog : "";

    broadcastJson(doc);
}

void WebManager::broadcastOtaProgress(int progress) {
    JsonDocument doc;
    doc["type"] = "ota_progress";
    doc["progress"] = progress;
    broadcastJson(doc);
}

void WebManager::broadcastJson(const JsonDocument& doc) {
    String output;
    serializeJson(doc, output);
    _ws.textAll(output); // Send JSON string to all connected WebSocket clients
}

void WebManager::otaCheckVersionTask(void* pvParameters) {
    WebManager* self = static_cast<WebManager*>(pvParameters);
    self->fetchOtaVersionInfo();
    self->broadcastOtaStatus();
    self->_isCheckingOta = false;
    vTaskDelete(NULL);
}

void WebManager::otaDownloadTask(void* pvParameters) {
    WebManager* self = static_cast<WebManager*>(pvParameters);
    self->downloadAndApplyOta();
    self->_isDownloadingOta = false;
    vTaskDelete(NULL);
}

bool WebManager::fetchOtaVersionInfo() {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);   // TLS 소켓 타임아웃 10초

    http.begin(client, OTA_VERSION_URL);
    http.setTimeout(10000);  // HTTP 응답 타임아웃 10초
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String body = http.getString();
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            xSemaphoreTake(_otaDataMutex, portMAX_DELAY);
            _latestOtaVersion = doc["version"].as<String>();
            _otaChangeLog = doc["notes"].as<String>();
            _otaFirmwareUrl = doc["url"].as<String>();
            _otaUpdateAvailable = isVersionNewer(_latestOtaVersion, _currentFirmwareVersion);
            xSemaphoreGive(_otaDataMutex);
            http.end();
            return true;
        }
    }
    http.end();
    return false;
}

void WebManager::downloadAndApplyOta() {
    esp_task_wdt_add(NULL);

    JsonDocument doc;
    doc["type"] = "ota_result";

    if (WiFi.status() != WL_CONNECTED) {
        doc["success"] = false;
        doc["msg"] = "OTA Failed: No Internet Connection.";
    } else {
        xSemaphoreTake(_otaDataMutex, portMAX_DELAY);
        String firmwareUrl = _otaFirmwareUrl;
        xSemaphoreGive(_otaDataMutex);

        if (firmwareUrl.isEmpty()) {
            doc["success"] = false;
            doc["msg"] = "OTA Failed: No firmware URL. Check version first.";
            broadcastJson(doc);
            esp_task_wdt_delete(NULL);
            return;
        }

        HTTPClient http;
        WiFiClientSecure client;
        client.setInsecure();

        http.begin(client, firmwareUrl);
        http.setTimeout(30000);  // 다운로드 HTTP 타임아웃 30초
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            size_t updateSize = (contentLength > 0) ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN;
            if (Update.begin(updateSize)) {
                Log::Info("OTA: Starting download. Size: %d bytes.", contentLength);
                WiFiClient *stream = http.getStreamPtr();
                size_t written = 0;
                int lastProgress = -1;
                uint8_t buff[1024] = { 0 };
                bool writeFailed = false;

                while (http.connected() && (contentLength <= 0 || written < (size_t)contentLength)) {
                    esp_task_wdt_reset();
                    size_t len = stream->readBytes(buff, sizeof(buff));
                    if (len > 0) {
                        if (Update.write(buff, len) != len) {
                            writeFailed = true;
                            break;
                        }
                        written += len;
                        if (contentLength > 0) {
                            int progress = (int)(((float)written / (float)contentLength) * 100);
                            if (progress > lastProgress) {
                                broadcastOtaProgress(progress);
                                lastProgress = progress;
                            }
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(1));
                }

                if (writeFailed) {
                    doc["success"] = false;
                    doc["msg"] = "Write error: " + String(Update.errorString());
                    Update.abort();
                } else if ((contentLength <= 0 || written == (size_t)contentLength) && Update.end(true)) {
                    _otaUpdateDownloaded = true;
                    if (_modeManager) _modeManager->setUpdateDownloaded(true);
                    doc["success"] = true;
                    doc["msg"] = "Download complete! Update will be applied on exit.";
                    Log::Info("OTA: Download successful.");
                } else {
                    doc["success"] = false;
                    doc["msg"] = "Update failed: " + String(Update.errorString());
                    Update.abort();
                }
            } else {
                doc["success"] = false;
                doc["msg"] = "Not enough space or invalid content length. Error: " + String(Update.getError());
            }
        } else {
            doc["success"] = false;
            doc["msg"] = "Failed to download. HTTP Error: " + String(httpCode);
        }
        http.end();
    }

    broadcastJson(doc);
    esp_task_wdt_delete(NULL);
}

void WebManager::setupLogBroadcaster() {
    Log::setWebSocketLogSender([this](const char* level, const String& msg) {
        if (_isServerRunning.load() && strcmp(level, "TEST") == 0) {
            JsonDocument doc;
            doc["type"] = "log";
            doc["ts"] = millis();
            doc["msg"] = msg;

            broadcastJson(doc);
        }
    });
}

String WebManager::getPageHeader(const String& title, const char* i18nKey) {
    String html = F("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    html += "<title>" + title + "</title>";
    html += F(R"rawliteral(<style>
        @import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap');

        body {
            font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
            margin: 0;
            padding: 20px 10px;
            background-color: #0b0b0f;
            background-image: radial-gradient(circle at 50% 0%, #1e1b4b 0%, #0b0b0f 70%);
            color: #f3f4f6;
            text-align: center;
            min-height: 100vh;
            box-sizing: border-box;
        }

        .container {
            max-width: 550px;
            margin: 40px auto;
            position: relative;
            background: rgba(255, 255, 255, 0.02);
            padding: 30px 24px;
            border-radius: 24px;
            border: 1px solid rgba(255, 255, 255, 0.08);
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            box-shadow: 0 20px 50px rgba(0, 0, 0, 0.4);
            text-align: center;
        }
        
        h1 {
            font-size: 26px;
            font-weight: 700;
            margin-top: 0;
            margin-bottom: 25px;
            background: linear-gradient(135deg, #a78bfa 0%, #8b5cf6 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            letter-spacing: -0.5px;
        }
        
        h2, h3 {
            color: #e5e7eb;
            font-weight: 600;
            margin-top: 0;
            margin-bottom: 12px;
            font-size: 18px;
            letter-spacing: -0.2px;
        }
        
        .card {
            background: rgba(255, 255, 255, 0.015);
            padding: 20px;
            margin-bottom: 20px;
            border-radius: 16px;
            border: 1px solid rgba(255, 255, 255, 0.05);
            box-shadow: inset 0 1px 1px rgba(255, 255, 255, 0.03);
            text-align: center;
        }
        
        .btn {
            display: inline-flex;
            align-items: center;
            justify-content: center;
            background: linear-gradient(135deg, #6d28d9 0%, #5b21b6 100%);
            color: #ffffff;
            padding: 12px 24px;
            margin: 8px 4px;
            text-decoration: none;
            border: none;
            border-radius: 12px;
            cursor: pointer;
            font-size: 15px;
            font-weight: 600;
            min-width: 160px;
            box-shadow: 0 4px 12px rgba(109, 40, 217, 0.2);
            transition: all 0.25s cubic-bezier(0.4, 0, 0.2, 1);
        }
        
        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(109, 40, 217, 0.35);
            background: linear-gradient(135deg, #7c3aed 0%, #6d28d9 100%);
        }
        
        .btn:active {
            transform: translateY(0);
        }
        
        .btn:disabled {
            background: #4b5563;
            color: #9ca3af;
            box-shadow: none;
            cursor: not-allowed;
            transform: none;
        }
        
        .btn-danger {
            background: linear-gradient(135deg, #dc2626 0%, #b91c1c 100%);
            box-shadow: 0 4px 12px rgba(220, 38, 38, 0.2);
        }
        
        .btn-danger:hover {
            background: linear-gradient(135deg, #ef4444 0%, #dc2626 100%);
            box-shadow: 0 6px 20px rgba(220, 38, 38, 0.35);
        }
        
        .btn-success {
            background: linear-gradient(135deg, #059669 0%, #047857 100%);
            box-shadow: 0 4px 12px rgba(5, 150, 105, 0.2);
        }
        
        .btn-success:hover {
            background: linear-gradient(135deg, #10b981 0%, #059669 100%);
            box-shadow: 0 6px 20px rgba(5, 150, 105, 0.35);
        }
        
        input, select {
            width: calc(100% - 28px);
            padding: 12px 14px;
            margin: 10px 0;
            background: rgba(0, 0, 0, 0.3);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 12px;
            color: #ffffff;
            font-size: 15px;
            text-align: center;
            outline: none;
            transition: all 0.2s ease;
        }
        
        input:focus, select:focus {
            border-color: #a78bfa;
            box-shadow: 0 0 0 3px rgba(167, 139, 250, 0.2);
            background: rgba(0, 0, 0, 0.4);
        }
        
        .hidden {
            display: none;
        }
        
        .form-group {
            margin-bottom: 20px;
            text-align: left;
        }
        
        .form-group label {
            display: block;
            margin-bottom: 6px;
            font-size: 14px;
            color: #9ca3af;
            font-weight: 500;
        }
        
        /* Floating Message Toast */
        .message-box {
            padding: 14px 20px;
            border-radius: 12px;
            margin-top: 15px;
            text-align: center;
            transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
            position: fixed;
            top: 20px;
            left: 50%;
            transform: translate(-50%, -20px);
            z-index: 1000;
            width: 85%;
            max-width: 420px;
            display: none;
            opacity: 0;
            box-shadow: 0 10px 30px rgba(0, 0, 0, 0.5);
            font-size: 14px;
            font-weight: 500;
        }
        
        .message-box.show {
            display: block;
            opacity: 1;
            transform: translate(-50%, 0);
        }
        
        .message-info {
            background-color: #1e3a8a;
            border: 1px solid #2563eb;
            color: #bfdbfe;
        }
        
        .message-success {
            background-color: #064e3b;
            border: 1px solid #059669;
            color: #a7f3d0;
        }
        
        .message-error {
            background-color: #7f1d1d;
            border: 1px solid #dc2626;
            color: #fca5a5;
        }
        
        .changelog {
            text-align: left;
            background: rgba(0, 0, 0, 0.4);
            padding: 15px;
            border-radius: 12px;
            margin-bottom: 15px;
            border: 1px solid rgba(255, 255, 255, 0.05);
            white-space: pre-wrap;
            font-size: 13px;
            line-height: 1.5;
            color: #d1d5db;
        }
        
        .progress-bar {
            width: 90%;
            max-width: 350px;
            background-color: rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            overflow: hidden;
            margin: 15px auto;
            border: 1px solid rgba(255, 255, 255, 0.05);
        }
        
        .progress-bar-inner {
            height: 14px;
            width: 0%;
            background: linear-gradient(90deg, #10b981, #059669);
            color: white;
            text-align: center;
            line-height: 14px;
            font-size: 10px;
            font-weight: 700;
            transition: width 0.2s ease;
        }
        
        .notice {
            font-size: 13px;
            color: #f87171;
            margin-top: 10px;
            font-weight: 500;
        }
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
    <script src='/i18n.js'></script>
    </head><body><div class='container'>)rawliteral");
    html += F("<div class='lang-switcher' id='lang-sw'><button class='lang-cur' id='lang-cur-btn' onclick='toggleLangMenu(event)'>🌐 EN &#9662;</button><div class='lang-drop'><div class='lang-drop-label'>Language</div><button class='lang-opt' data-lang='0' onclick='setLang(0)'>EN — English</button><button class='lang-opt' data-lang='1' onclick='setLang(1)'>한국어</button><button class='lang-opt' data-lang='2' onclick='setLang(2)'>中文</button><button class='lang-opt' data-lang='3' onclick='setLang(3)'>日本語</button><button class='lang-opt' data-lang='4' onclick='setLang(4)'>DE — Deutsch</button><button class='lang-opt' data-lang='5' onclick='setLang(5)'>ES — Español</button><button class='lang-opt' data-lang='6' onclick='setLang(6)'>FR — Français</button></div></div>");
    html += F("<h1");
    if (i18nKey) { html += F(" data-i18n='"); html += i18nKey; html += F("'"); }
    html += F(">");
    html += title;
    html += F("</h1>");
    return html;
}

String WebManager::getPageFooter(bool showHomeButton) {
    String html;
    if (showHomeButton) html += F("<p style='margin-top:25px;'><a href='/' class='btn' data-i18n='back_home'>Back to Home</a></p>");
    html += F("</div></body></html>");
    return html;
}
