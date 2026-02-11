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

    // スイッチピン設定
    pinMode(SW_L_PIN, INPUT_PULLUP);
    pinMode(SW_R_PIN, INPUT_PULLUP);
    pinMode(SW_P_PIN, INPUT_PULLUP);

    // SD初期化（リトライ付き）
    delay(100);
    bool sd_ok = false;
    for (int retry = 0; retry < 5; retry++) {
        if (SD.begin(4)) { sd_ok = true; break; }
        Serial.printf("SD init retry %d/5...\n", retry + 1);
        delay(200);
    }
    if (!sd_ok) {
        Serial.println("SD init failed!");
        canvas.createCanvas(540, 960);
        canvas.setTextSize(32);
        canvas.drawString("SD Card Error!", 10, 100);
        canvas.pushCanvas(0, 0, UPDATE_MODE_DU4);
        while (1) delay(1000);
    }
    Serial.println("SD initialized");
    sd_healthy = true;

    // 設定読み込み
    loadConfig();

    // キャンバス作成
    canvas.createCanvas(540, 960);
    canvas.setTextDatum(TL_DATUM);

    // フォント読み込み
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
        setenv("TZ", TZ_JST, 1);
        tzset();
        configTime(0, 0, "pool.ntp.org", "time.google.com");

        canvas.drawString("WiFi OK! Syncing time...", 270, 280);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

        // NTP同期待ち
        time_t now = 0;
        int retry = 0;
        while (now < 1700000000 && retry < 20) {
            delay(500);
            now = time(nullptr);
            retry++;
        }
        Serial.printf("NTP sync: %ld (retry: %d)\n", now, retry);

        // ICS取得
        canvas.drawString("Fetching calendar...", 270, 320);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        fetchAndUpdate();
        last_fetch = time(nullptr);
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
        int poll_interval = (config.ics_poll_min * 60) * (1 + min(fetch_fail_count, 5));
        if (now != (time_t)-1 && (now - last_fetch) >= poll_interval) {
            if (!sd_healthy) {
                Serial.println("ICS fetch skipped - SD unhealthy");
                last_fetch = now;
            } else {
                if (WiFi.status() != WL_CONNECTED) {
                    if (!connectWiFi()) { last_fetch = now; fetch_fail_count++; }
                }
                if (WiFi.status() == WL_CONNECTED) {
                    fetchAndUpdate();
                    if (ui_state == UI_LIST) { scrollToToday(); drawList(); }
                }
            }
        }
    }

    delay(1);
}
