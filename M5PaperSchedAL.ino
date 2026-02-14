/*******************************************************************************
 * M5Paper ICS Alarm with MIDI (Unit Synth)
 * 
 * Features:
 * - ICS calendar fetch with Basic Auth support (streaming parser)
 * - !...! alarm marker detection (e.g. !-10!, !+5<song.mid@15*2!)
 * - MIDI playback via custom player (SysEx support)
 * - Touch UI: List → Detail → Playing
 * - Settings menu via L/R/P switches
 * - On-screen keyboard for WiFi/ICS configuration
 * - SD card health monitoring and auto-reinit
 * 
 * Hardware:
 * - M5Paper v1.1
 * - Unit Synth on Port B (GPIO 26 TX)
 * 
 * File structure:
 *   types.h          - Structs, enums, constants
 *   globals.h        - Extern declarations, function prototypes
 *   globals.cpp      - Global variable definitions
 *   config.cpp       - Config load/save (JSON)
 *   utf8_utils.cpp   - UTF-8 string processing
 *   sd_utils.cpp     - SD health check, MIDI file scan
 *   network.cpp      - WiFi, ntfy, MIDI download
 *   midi_player.cpp  - MIDI playback control
 *   ics_parser.cpp   - Streaming ICS parser + fetch
 *   ui_common.cpp    - Shared UI utilities
 *   ui_list.cpp      - List view
 *   ui_detail.cpp    - Detail & Playing views
 *   ui_settings.cpp  - Settings menu
 *   ui_keyboard.cpp  - On-screen keyboard
 *   input_handler.cpp - Touch/switch/alarm handling
 ******************************************************************************/

#include "globals.h"
#include <SD.h>
#include <time.h>

//==============================================================================
// セットアップ
//==============================================================================
void setup() {
    Serial.begin(115200);
    Serial.printf("\n=== M5Paper Alarm starting... ver.%s ===\n", BUILD_VERSION);

    M5.begin();
    M5.TP.SetRotation(90);
    M5.EPD.SetRotation(90);
    M5.EPD.Clear(true);

    // イベント配列をPSRAMに確保（M5.begin後＝PSRAM初期化後）
    Serial.printf("PSRAM: %dKB free\n", ESP.getFreePsram() / 1024);
    events = (EventItem*)ps_calloc(MAX_EVENTS, sizeof(EventItem));
    Serial.printf("Events array: %d x %d = %dKB in PSRAM (%s)\n",
                  MAX_EVENTS, (int)sizeof(EventItem),
                  (int)(MAX_EVENTS * sizeof(EventItem) / 1024),
                  events ? "OK" : "FAILED");
    if (!events) {
        Serial.println("FATAL: PSRAM allocation failed!");
        while(1) delay(1000);
    }

    // スイッチピン設定
    pinMode(SW_L_PIN, INPUT_PULLUP);
    pinMode(SW_R_PIN, INPUT_PULLUP);
    pinMode(SW_P_PIN, INPUT_PULLUP);

    // SD初期化
    delay(100);
    if (!initSD()) {
        Serial.println("SD init failed! Waiting for user action...");
        canvas.createCanvas(540, 960);
        canvas.setTextSize(3);  // デフォルトフォント用サイズ
        canvas.setTextColor(15);
        canvas.drawString("SD Card Error!", 10, 100);
        canvas.drawString("Remove & reinsert SD card", 10, 150);
        canvas.drawString("Then press P button", 10, 200);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

        // Pボタン押下でリトライ
        while (true) {
            if (digitalRead(SW_P_PIN) == LOW) {
                delay(50);
                Serial.println("Retrying SD init...");
                if (initSD()) {
                    Serial.println("SD recovered!");
                    break;
                }
                canvas.fillCanvas(0);
                canvas.drawString("SD still failed...", 10, 100);
                canvas.drawString("Remove & reinsert SD card", 10, 150);
                canvas.drawString("Then press P button", 10, 200);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
                delay(500);
            }
            delay(50);
        }
    }
    Serial.println("SD initialized");

    // 設定読み込み
    loadConfig();

    // キャンバス作成
    canvas.createCanvas(540, 960);
    canvas.setTextDatum(TL_DATUM);

    // フォント読み込み
    waitEPDReady();
    if (SD.exists(FONT_PATH)) {
        canvas.loadFont(FONT_PATH, SD);
        canvas.createRender(64, 256);
        canvas.createRender(48, 256);
        canvas.createRender(40, 256);
        canvas.createRender(32, 256);
        canvas.createRender(28, 256);
        canvas.createRender(26, 256);
        canvas.createRender(24, 256);
        canvas.createRender(22, 256);
        canvas.createRender(20, 256);
        Serial.println("Font loaded");
    } else {
        Serial.println("Font not found, using default");
    }

    // MIDI UART初期化
    Serial2.begin(config.midi_baud, SERIAL_8N1, -1, port_tx_pins[config.port_select]);
    Serial.printf("MIDI UART on GPIO %d @ %d baud\n",
                  port_tx_pins[config.port_select], config.midi_baud);

    // 起動画面
    canvas.fillCanvas(0);
    canvas.setTextColor(15);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(32);
    canvas.drawString("M5Paper Alarm", 270, 180);
    canvas.setTextSize(24);
    canvas.drawString("Connecting WiFi...", 270, 240);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    // WiFi接続
    if (connectWiFi()) {
        configTzTime(TZ_JST, "pool.ntp.org", "time.google.com", "ntp.nict.jp");

        canvas.drawString("WiFi OK! Syncing time...", 270, 280);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

        // NTP同期待ち
        time_t now = 0;
        int retry = 0;
        while (retry < 40) {  // 最大20秒
            delay(500);
            now = time(nullptr);
            if (now > 1700000000) break;  // 2023年以降なら同期完了
            retry++;
        }
        if (now < 1700000000) {
            Serial.printf("NTP sync FAILED after %d retries (time=%ld)\n", retry, now);
            // 時刻未同期 → ICS取得しても日付フィルタで全件弾かれるのでスキップ
            canvas.drawString("NTP sync failed, will retry", 270, 320);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            delay(1000);
        } else {
            Serial.printf("NTP sync OK: %ld (retry: %d)\n", now, retry);

            // ICS取得
            canvas.drawString("Fetching calendar...", 270, 320);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            fetchAndUpdate();
            last_fetch = time(nullptr);
            Serial.printf("Initial fetch complete: %d events loaded\n", event_count);
        }
    } else {
        canvas.drawString("WiFi Failed", 270, 280);
        canvas.drawString("Press P for settings", 270, 320);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        delay(2000);
    }

    // 一覧表示
    ui_state = UI_LIST;
    selected_event = 0;
    page_start = 0;
    last_interaction_ms = millis();
    last_sd_check_ms = millis();
    scrollToToday();
    drawList();
}

//==============================================================================
// 安全リブート（直近アラームがなければリブート）
//==============================================================================
void safeReboot() {
    time_t now = time(nullptr);
    for (int i = 0; i < event_count; i++) {
        if (events[i].has_alarm && !events[i].triggered) {
            long remain = (long)(events[i].alarm_time - now);
            if (remain > 0 && remain < 300) {  // 5分以内
                Serial.printf("REBOOT DEFERRED: alarm '%s' in %ld sec\n",
                              events[i].summary(), remain);
                reboot_pending = true;  // アラーム後にリブート
                return;
            }
        }
    }
    delay(100);
    ESP.restart();
}

//==============================================================================
// メインループ
//==============================================================================
void loop() {
    // MIDI再生更新
    updateMidiPlayback();

    // スイッチチェック（50ms間隔）
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

    // アラームチェック
    if (ui_state != UI_PLAYING && ui_state != UI_SETTINGS && ui_state != UI_KEYBOARD) {
        checkAlarms();
    }

    // SDカード健全性チェック（5分ごと、再生中以外）
    if (ui_state != UI_PLAYING && (millis() - last_sd_check_ms) > SD_CHECK_INTERVAL_MS) {
        last_sd_check_ms = millis();
        if (!checkSDHealth()) {
            Serial.println("SD_CHECK: Health check failed, attempting reinit");
            sd_healthy = false;
            reinitSD();
            if (sd_healthy && ui_state == UI_LIST) drawList();
        } else {
            sd_healthy = true;
            Serial.printf("SD_CHECK: OK (heap: %d)\n", ESP.getFreeHeap());
        }
    }

    // 操作なし3分以上 かつ UI_LIST の場合、毎分自動リフレッシュ
    {
        time_t now_t = time(nullptr);
        bool idle = (millis() - last_interaction_ms) > 180000;
        if (idle && ui_state == UI_LIST && now_t != (time_t)-1 &&
            (now_t - last_auto_refresh) >= 60) {
            last_auto_refresh = now_t;
            int old_page = page_start;
            scrollToToday();
            if (page_start != old_page) {
                Serial.printf("AUTO-REFRESH: page %d->%d\n", old_page, page_start);
                drawList();
            }
        }
    }

    // 定期ICS更新
    if (ui_state != UI_PLAYING) {
        time_t now = time(nullptr);
        int poll_interval = (event_count == 0) ? 30 : (config.ics_poll_min * 60);
        if (now != (time_t)-1 && (now - last_fetch) >= poll_interval) {
            if (!sd_healthy) {
                Serial.println("ICS fetch skipped - SD unhealthy");
                last_fetch = now;
            } else {
                // ヒープ断片化 → リブート（アラーム保護付き）
                if ((int)ESP.getMaxAllocHeap() < 45000) {
                    Serial.printf("*** REBOOT: heap fragmented, maxBlock:%d ***\n",
                                  ESP.getMaxAllocHeap());
                    safeReboot();
                }

                // WiFi未接続なら再接続
                if (WiFi.status() != WL_CONNECTED) connectWiFi();

                // フェッチ
                if (WiFi.status() == WL_CONNECTED) {
                    int before = event_count;
                    fetchAndUpdate();

                    if (event_count == 0 && before > 0) {
                        // 失敗 → WiFiリセットして再フェッチ
                        Serial.println("Fetch lost all events, WiFi reset + retry");
                        connectWiFi();
                        if (WiFi.status() == WL_CONNECTED) {
                            fetchAndUpdate();
                        }
                    }

                    if (event_count == 0 && before > 0) {
                        // 2回失敗 → リブート（アラーム保護付き）
                        Serial.println("*** REBOOT: fetch failed twice ***");
                        safeReboot();
                    }

                    Serial.printf("Periodic fetch: %d -> %d events\n", before, event_count);
                    if (ui_state == UI_LIST) { scrollToToday(); drawList(); }
                }
            }
        }
    }

    delay(1);
}
