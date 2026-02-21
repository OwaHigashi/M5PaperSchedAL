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
#include <esp_system.h>

// ★ WiFiClientSecure + mbedTLS のSSLハンドシェイクが内部で3-4KBのスタックを使用
//    デフォルト8KBでは doFetch() → parseICSStream() → stream->read() のコールチェーンで溢れる
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ダブルバッファ実体定義（PSRAM上に確保、setup()でps_calloc）
EventItem* events_buf_a = nullptr;
EventItem* events_buf_b = nullptr;

//==============================================================================
// セットアップ
//==============================================================================
void setup() {
    // ★ サイレントリスタート判定
    //    ESP_RST_SW = ESP.restart() = safeReboot()のみ
    //    RTC_DATA_ATTR はM5Paperのブートローダーにクリアされるため使用不可
    esp_reset_reason_t reason = esp_reset_reason();
    bool silent_mode = (reason == ESP_RST_SW);

    Serial.begin(115200);

    M5.begin();
    M5.TP.SetRotation(90);
    M5.EPD.SetRotation(90);

    // ★ M5.begin後にログ出力（Serial再初期化後でも確実に出る）
    Serial.printf("\n=== M5Paper Alarm starting... ver.%s ===\n", BUILD_VERSION);
    const char* reasons[] = {
        "UNKNOWN","POWERON","EXT","SW","PANIC","INT_WDT",
        "TASK_WDT","WDT","DEEPSLEEP","BROWNOUT","SDIO"
    };
    Serial.printf("Reset reason: %s (%d), silent: %s\n",
                  (reason < 11) ? reasons[reason] : "?", reason,
                  silent_mode ? "YES" : "NO");

    if (!silent_mode) {
        M5.EPD.Clear(true);  // コールドブート時のみ画面クリア
    } else {
        Serial.println("*** Silent restart - e-paper display preserved ***");
    }

    // イベント配列をPSRAMにダブルバッファで確保（M5.begin後＝PSRAM初期化後）
    Serial.printf("PSRAM: %dKB free\n", ESP.getFreePsram() / 1024);
    Serial.printf("[MEM] After M5.begin: heap:%d maxBlock:%d\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    events_buf_a = (EventItem*)ps_calloc(MAX_EVENTS, sizeof(EventItem));
    events_buf_b = (EventItem*)ps_calloc(MAX_EVENTS, sizeof(EventItem));
    events = events_buf_a;
    int bufKB = (int)(MAX_EVENTS * sizeof(EventItem) / 1024);
    Serial.printf("Events double buffer: %d x %d = %dKB x2 = %dKB in PSRAM (%s)\n",
                  MAX_EVENTS, (int)sizeof(EventItem), bufKB, bufKB * 2,
                  (events_buf_a && events_buf_b) ? "OK" : "FAILED");
    // PSRAM検証: アドレスが0x3F800000以降ならPSRAM、0x3FF00000以降ならDRAM
    Serial.printf("  buf_a @ %p, buf_b @ %p (%s)\n",
                  events_buf_a, events_buf_b,
                  ((uintptr_t)events_buf_a >= 0x3F800000 && (uintptr_t)events_buf_a < 0x3FF00000) ? "PSRAM OK" : "DRAM!!");
    Serial.printf("[MEM] After ps_calloc: heap:%d maxBlock:%d psram:%dKB\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram() / 1024);
    if (!events_buf_a || !events_buf_b) {
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

    // MIDI UART初期化
    Serial2.begin(config.midi_baud, SERIAL_8N1, -1, port_tx_pins[config.port_select]);
    Serial.printf("MIDI UART on GPIO %d @ %d baud\n",
                  port_tx_pins[config.port_select], config.midi_baud);

    // キャンバス作成
    canvas.createCanvas(540, 960);
    canvas.setTextDatum(TL_DATUM);

    // ★ フォント読み込み（loadFont + createRender をまとめて実行）
    //    キャッシュを256→64に縮小 → DRAM断片化を抑え、maxBlockを温存
    //    未使用サイズ(40,64)削除、使用サイズ(30,32,48)追加
    waitEPDReady();
    if (SD.exists(FONT_PATH)) {
        canvas.loadFont(FONT_PATH, SD);
        canvas.createRender(48, 64);
        canvas.createRender(32, 64);
        canvas.createRender(30, 64);
        canvas.createRender(28, 64);
        canvas.createRender(26, 64);
        canvas.createRender(24, 64);
        canvas.createRender(22, 64);
        canvas.createRender(20, 64);
        canvas.createRender(18, 64);
        Serial.printf("Font loaded (heap:%d maxBlock:%d)\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    } else {
        Serial.println("Font not found, using default");
    }

    // 起動画面（コールドブート時のみ）
    if (!silent_mode) {
        canvas.fillCanvas(0);
        canvas.setTextColor(15);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextSize(32);
        canvas.drawString("M5Paper Alarm", 270, 180);
        canvas.setTextSize(24);
        char verBuf[64];
        snprintf(verBuf, sizeof(verBuf), "ver.%s", BUILD_VERSION);
        canvas.drawString(verBuf, 270, 240);
        canvas.drawString("Connecting WiFi...", 270, 300);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        waitEPDReady();
    }

    Serial.printf("Pre-WiFi memory: heap:%d maxBlock:%d\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // WiFi + NTP + 初回fetch
    if (connectWiFi()) {
        configTzTime(TZ_JST, "pool.ntp.org", "time.google.com", "ntp.nict.jp");

        if (!silent_mode) {
            canvas.drawString("WiFi OK!", 270, 360);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        }

        // NTP同期待ち
        time_t now = 0;
        int retry = 0;
        while (retry < 40) {
            delay(500);
            now = time(nullptr);
            if (now > 1700000000) break;
            retry++;
        }
        if (now < 1700000000) {
            Serial.printf("NTP sync FAILED after %d retries (time=%ld)\n", retry, now);
            if (!silent_mode) {
                canvas.drawString("NTP sync failed", 270, 420);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            }
            delay(1000);
        } else {
            Serial.printf("NTP sync OK: %ld (retry: %d)\n", now, retry);

            if (!silent_mode) {
                canvas.drawString("Fetching calendar...", 270, 420);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            }
            fetchAndUpdate();
            last_fetch = time(nullptr);
            Serial.printf("Initial fetch complete: %d events loaded\n", event_count);
        }
    } else {
        if (!silent_mode) {
            canvas.drawString("WiFi Failed", 270, 360);
            canvas.drawString("P長押し → 設定", 270, 420);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        }
        delay(2000);
    }

    // canvas状態リセット → リスト描画
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(15);
    canvas.setTextSize(26);

    // 一覧表示
    ui_state = UI_LIST;
    selected_event = 0;
    page_start = 0;
    last_interaction_ms = millis();
    last_sd_check_ms = millis();
    scrollToToday();
    Serial.printf("[DRAW] drawList: events=%d page_start=%d selected=%d silent=%s\n",
                  event_count, page_start, selected_event,
                  silent_mode ? "YES(skip push)" : "NO");
    drawList(false, silent_mode);  // サイレント時はpushCanvasスキップ
    Serial.println("[DRAW] drawList complete");
}

//==============================================================================
// 安全リブート（直近アラームがなければリブート）
//==============================================================================
void safeReboot() {
    // MIDI再生中は延期
    if (midi_playing) {
        Serial.println("REBOOT DEFERRED: MIDI playing");
        reboot_pending = true;
        return;
    }
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
    Serial.println("=== Silent reboot ===");
    Serial.flush();
    delay(100);
    ESP.restart();
}

//==============================================================================
// メインループ
//==============================================================================
void loop() {
    unsigned long loop_start = millis();

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
            if (p.x > 0 && p.y > 0) {
                unsigned long t = millis();
                handleTouch(p.x, p.y);
                Serial.printf("Touch(%d,%d) handled in %lu ms\n", p.x, p.y, millis() - t);
            }
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
        int poll_interval = debug_fetch ? 30 : ((event_count == 0) ? 30 : (config.ics_poll_min * 60));
        if (now != (time_t)-1 && (now - last_fetch) >= poll_interval) {
            if (!sd_healthy) {
                Serial.println("ICS fetch skipped - SD unhealthy");
                last_fetch = now;
            } else {
                // ヒープ断片化 → リブート（String排除後は閾値を大幅引き下げ）
                if ((int)ESP.getMaxAllocHeap() < 10000) {
                    Serial.printf("*** REBOOT: heap fragmented, maxBlock:%d ***\n",
                                  ESP.getMaxAllocHeap());
                    safeReboot();
                }

                // WiFi未接続なら再接続
                if (WiFi.status() != WL_CONNECTED) connectWiFi();

                // フェッチ（ダブルバッファ: 失敗しても旧データ保持）
                if (WiFi.status() == WL_CONNECTED) {
                    int before = event_count;
                    bool changed = fetchAndUpdate();
                    Serial.printf("Periodic fetch: %d -> %d events\n", before, event_count);
                    if (changed && ui_state == UI_LIST) { scrollToToday(); drawList(false, false, true); }
                }
            }
        }
    }

    unsigned long loop_dur = millis() - loop_start;
    if (loop_dur > 100) Serial.printf("[LOOP] slow iteration: %lu ms\n", loop_dur);
    delay(1);
}
