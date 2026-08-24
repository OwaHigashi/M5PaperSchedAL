/*******************************************************************************
 * M5Paper Schedule Display — thin client (v100)
 *
 * 役割分担:
 *   ホスト(Ubuntu, server/)  ICS取得(HTTPS)・RRULE展開・アラーム判定/状態・ntfy・MIDI取得
 *   M5Paper(本ファーム)       画面表示・鳴動・タッチ/スイッチ操作の報告 だけ
 *
 * 端末↔ホストは LAN 内の素の HTTP/1.1 (SSLなし)。
 *   - 5秒毎のハートビート(Active Sensing)で生存報告・時刻合わせ・コマンド受信
 *   - テーブル版(rev)が変わったら GET /api/v1/events で全件再取得 (NDJSON)
 *   - 鳴動完了は POST /api/v1/alarm/ack。届かなければ LittleFS に保持して再送
 *   - ホスト不達でも手元のテーブルで時刻通りに鳴動する
 *   - v102: 予定テーブルを LittleFS (/events.cache) に、時刻を BM8563 RTC に永続化。
 *     ホスト不達のまま再起動(2時間ルール含む)しても表示・鳴動を自律継続する
 *
 * SDカードは使わない。フォント/設定/MIDI は内蔵フラッシュ(LittleFS, data/ → uploadfs)。
 *
 * File structure:
 *   types.h / globals.h / globals.cpp   定義・グローバル
 *   config.cpp        LittleFS /config.json (WiFi, サーバ, MIDIポート)
 *   fs_utils.cpp      LittleFS 初期化・MIDI一覧
 *   network.cpp       WiFi・素のHTTPクライアント・MIDI取得・スクリーンショット送信
 *   sync_client.cpp   ホスト同期・ハートビート・コマンド・時刻合わせ・ACK
 *   logger.cpp        ログ(ホストへ転送)・操作イベントキュー
 *   midi_player.cpp   MIDI再生
 *   input_handler.cpp スイッチ/タッチ/アラーム発火
 *   ui_*.cpp          画面
 ******************************************************************************/

#include "globals.h"
#include <time.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

EventItem* events_buf_a = nullptr;
EventItem* events_buf_b = nullptr;

static void bootMsg(const char* s, int y) {
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(24);
    canvas.drawString(s, 270, y);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    waitEPDReady();
}

//==============================================================================
// セットアップ
//==============================================================================
void setup() {
    esp_reset_reason_t reason = esp_reset_reason();
    bool silent_mode = (reason == ESP_RST_SW);

    Serial.begin(115200);
    M5.begin(true, false, true, true, true);   // touch, SD=off, Serial, BatADC, I2C
    M5.TP.SetRotation(90);
    M5.EPD.SetRotation(90);
    M5.SHT30.Begin();

    Serial.printf("\n=== M5Paper Sched thin-client ver.%s ===\n", BUILD_VERSION);
    const char* reasons[] = {"UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT","TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO"};
    Serial.printf("Reset reason: %s (%d)\n", (reason < 11) ? reasons[reason] : "?", reason);

    if (!silent_mode) M5.EPD.Clear(true);

    // イベント配列 (PSRAM, ダブルバッファ)
    events_buf_a = (EventItem*)ps_calloc(MAX_EVENTS, sizeof(EventItem));
    events_buf_b = (EventItem*)ps_calloc(MAX_EVENTS, sizeof(EventItem));
    events = events_buf_a;
    Serial.printf("Events buffer: %d x %dB x2 in PSRAM (%s), psram free %uKB\n",
                  MAX_EVENTS, (int)sizeof(EventItem), (events_buf_a && events_buf_b) ? "OK" : "FAILED",
                  ESP.getFreePsram() / 1024);
    if (!events_buf_a || !events_buf_b) {
        Serial.println("FATAL: PSRAM allocation failed!");
        while (1) delay(1000);
    }

    pinMode(SW_L_PIN, INPUT_PULLUP);
    pinMode(SW_R_PIN, INPUT_PULLUP);
    pinMode(SW_P_PIN, INPUT_PULLUP);

    // 内蔵フラッシュ
    initFS();
    loadConfig();
    loadPendingAcks();
    setenv("TZ", config.tz, 1); tzset();

    // BM8563 RTCから時刻を仮復元 (ホスト同期が取れ次第、毎回上書き補正される)。
    // これによりホスト不達のまま再起動しても時刻を失わず鳴動を継続できる。
    rtcLoadTime();

    // MIDI UART
    Serial2.begin(config.midi_baud, SERIAL_8N1, -1, port_tx_pins[config.port_select]);

    // キャンバス・フォント
    canvas.createCanvas(540, 960);
    canvas.setTextWrap(false);
    canvas.setTextDatum(TL_DATUM);
    waitEPDReady();
    if (fs_ok && LittleFS.exists(FONT_PATH)) {
        canvas.loadFont(FONT_PATH, LittleFS);
        const int sizes[] = {48, 32, 30, 28, 26, 24, 22, 20, 18};
        for (int sz : sizes) canvas.createRender(sz, 64);
        Serial.printf("Font loaded (heap:%d maxBlock:%d)\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    } else {
        Serial.println("Font not found in LittleFS (/fonts/ipaexg.ttf) - run `pio run -t uploadfs`");
    }

    if (!silent_mode) {
        canvas.fillCanvas(0);
        canvas.setTextColor(15);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextSize(32);
        canvas.drawString("M5Paper Sched", 270, 180);
        char verBuf[64];
        snprintf(verBuf, sizeof(verBuf), "ver.%s  host %s:%d", BUILD_VERSION, config.server_host, config.server_port);
        canvas.setTextSize(24);
        canvas.drawString(verBuf, 270, 240);
        bootMsg("Connecting WiFi...", 300);
    }

    // WiFi → ホスト
    if (connectWiFi()) {
        if (!silent_mode) bootMsg("WiFi OK. ホストに接続中...", 360);
        bool hb = false;
        for (int i = 0; i < 5 && !hb; i++) { hb = sendHeartbeat(true); if (!hb) delay(1000); }
        if (hb) {
            if (!silent_mode) bootMsg("ホスト OK. 予定を取得中...", 420);
            fetchAndUpdate();
        } else {
            if (!silent_mode) bootMsg("ホストに接続できません (再試行します)", 420);
            delay(1500);
        }
    } else if (!silent_mode) {
        bootMsg("WiFi Failed — P → 設定", 360);
        delay(2000);
    }

    // ホストから予定を取得できなかった場合はローカルキャッシュから復元
    // (時刻はRTC復元済みなので、キャッシュの予定でそのまま鳴動できる)
    if (event_count == 0) loadEventsCache();

    {
        float tC = -99.0f;
        if (M5.SHT30.UpdateData() == 0) tC = M5.SHT30.GetTemperature();
        logLine("boot reset=%s(%d) ver=%s h=%d t=%.1f bat=%u wifi=%d host=%d ev=%d",
                (reason < 11) ? reasons[reason] : "?", (int)reason, BUILD_VERSION,
                (int)ESP.getFreeHeap(), tC, (unsigned)M5.getBatteryVoltage(), (int)WiFi.status(),
                host_online ? 1 : 0, event_count);
    }

    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(15);
    canvas.setTextSize(26);

    ui_state = UI_LIST;
    selected_event = 0;
    page_start = 0;
    last_interaction_ms = millis();
    scrollToToday();
    drawList(false, false);
    last_gc16_cleanup = time(nullptr);

    heartbeat_canvas.createCanvas(14, 14);

    esp_task_wdt_init(120, true);
    esp_task_wdt_add(NULL);
    Serial.println("Task WDT enabled (120s timeout)");
}

//==============================================================================
// メインループ
//==============================================================================
void loop() {
    unsigned long loop_start = millis();

    pollSerialCommands();
    updateMidiPlayback();

    if (millis() - last_switch_check > 50) {
        last_switch_check = millis();
        checkSwitches();
    }

    // タッチ処理
    static bool was_touched = false;
    if (M5.TP.available()) {
        M5.TP.update();
        bool is_touched = M5.TP.getFingerNum() > 0;
        if (was_touched && !is_touched) {
            tp_finger_t p = M5.TP.readFinger(0);
            if (p.x > 0 && p.y > 0) handleTouch(p.x, p.y);
        }
        was_touched = is_touched;
        M5.TP.flush();
    }

    // アラーム (一覧画面以外でも鳴らす。設定画面中に予定をすっぽ抜けさせない)
    if (ui_state != UI_KEYBOARD) checkAlarms();

    // 詳細画面: 30秒無操作で一覧に自動復帰
    if (ui_state == UI_DETAIL && (millis() - last_interaction_ms) > 30000) {
        ui_state = UI_LIST;
        partial_refresh_count = 0;
        waitEPDReady();
        drawList(false, false, false, true);
    }

    // ── ホスト通信 ──
    {
        bool was_online = host_online;
        long old_rev = host_rev;
        sendHeartbeat();                       // 間隔はホスト指定 (既定5秒)。再生中は間引き
        if (host_online && !was_online) retryPendingAcks();

        bool need_sync = false;
        if (host_online && host_rev >= 0) {
            if (host_rev != local_rev) need_sync = true;                                   // ホスト側で更新
            if (last_fetch > 0 && (time(nullptr) - last_fetch) >= full_sync_sec) need_sync = true;
            if (last_fetch == 0) need_sync = true;                                         // 起動時に取れていない
        }
        (void)old_rev;

        static unsigned long last_sync_try_ms = 0;
        if (need_sync && !midi_playing && ui_state != UI_KEYBOARD && millis() - last_sync_try_ms > 3000) {
            last_sync_try_ms = millis();
            bool changed = fetchAndUpdate();
            if (changed && ui_state == UI_LIST) {
                scrollToToday(); partial_refresh_count = 0;
                time_t now = time(nullptr); struct tm tmN; localtime_r(&now, &tmN);
                bool night = (tmN.tm_hour >= NIGHT_START_HOUR && tmN.tm_hour < NIGHT_END_HOUR);
                drawList(false, false, false, !night);
                if (night) last_gc16_cleanup = now;
            }
        }
        if (host_online) retryPendingAcks();

        // 鳴動状態(triggered)が変わったらキャッシュへ反映 (再生中は避ける)
        if (events_cache_dirty && !midi_playing) saveEventsCache();

        // ホスト不達が続く → WiFi張り直し → 最終的に再起動
        if (host_lost_since_ms != 0) {
            unsigned long lost = millis() - host_lost_since_ms;
            static unsigned long last_wifi_reset = 0;
            if (lost > HOST_LOST_WIFI_RESET_MS && millis() - last_wifi_reset > HOST_LOST_WIFI_RESET_MS) {
                last_wifi_reset = millis();
                logLine("host lost %lumin - wifi reconnect", lost / 60000UL);
                if (!midi_playing) connectWiFi();
            }
            if (lost > HOST_LOST_REBOOT_MS) {
                logLine("host lost %lumin - reboot", lost / 60000UL);
                safeReboot();
            }
        }
    }

    // 無操作3分以上 かつ 一覧: 毎分ヘッダ/▶の部分更新、20分毎にGC16掃除
    {
        time_t now_t = time(nullptr);
        bool idle = (millis() - last_interaction_ms) > 180000;
        if (idle && ui_state == UI_LIST && time_valid && (now_t - last_auto_refresh) >= 60) {
            last_auto_refresh = now_t;
            int old_page = page_start;
            scrollToToday();
            if (page_start != old_page) {
                partial_refresh_count = 0;
                drawList();
            } else if ((now_t - last_gc16_cleanup) >= (GC16_CLEANUP_MIN * 60)) {
                last_gc16_cleanup = now_t;
                partial_refresh_count = 0;
                drawList();
            } else {
                partialRefreshHeader();
                partialRefreshNextLine();
            }
        }
    }

    // ハートビート ● 明滅（一覧時、5秒毎。ホスト不達時は点滅しない＝目視で分かる）
    if (ui_state == UI_LIST && (millis() - last_heartbeat_ms) >= 5000) {
        last_heartbeat_ms = millis();
        heartbeat_visible = host_online ? !heartbeat_visible : true;
        heartbeat_canvas.fillCanvas(0);
        if (heartbeat_visible) {
            if (host_online) heartbeat_canvas.fillCircle(7, 7, 5, 15);
            else heartbeat_canvas.drawCircle(7, 7, 5, 15);    // 中空 = ホスト不達
        }
        heartbeat_canvas.pushCanvas(522, 4, UPDATE_MODE_DU);
    }

    esp_task_wdt_reset();
    unsigned long loop_dur = millis() - loop_start;
    if (loop_dur > 500) Serial.printf("[LOOP] slow iteration: %lu ms\n", loop_dur);
    delay(1);
}
