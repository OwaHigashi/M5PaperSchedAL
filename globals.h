#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <M5EPD.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "SimpleMIDIPlayer.h"
#include "types.h"

//==============================================================================
// グローバル変数 (extern宣言 — 実体は globals.cpp)
//==============================================================================

// ハードウェア・描画
extern M5EPD_Canvas canvas;

// 設定・データ
extern Config config;
extern EventItem* events;
extern EventItem* events_buf_a;
extern EventItem* events_buf_b;
extern int event_count;

// UI状態
extern UiState ui_state;
extern int selected_event;
extern int page_start;
extern int displayed_count;
extern int row_event_idx[MAX_EVENTS];
extern int detail_scroll;

// ボタン領域
extern ButtonArea btn_prev, btn_next, btn_today, btn_detail;

// 設定画面
extern int settings_cursor;

// 再生状態
extern int playing_event;
extern int playing_alarm_idx;
extern char playing_alarm_id[40];       // ホスト上のアラームID ("<eventid>-<+off>")
extern int play_repeat_remaining;
extern unsigned long play_start_ms;
extern int play_duration_ms;
extern char play_file_override[96];     // コマンド/テスト再生時のファイルパス

// キーボード
extern String keyboard_buffer;
extern int keyboard_target;
extern int keyboard_cursor;

// MIDIファイルリスト
extern String midi_files[32];
extern int midi_file_count;
extern int midi_select_cursor;

// ボーレート・ポート選択
extern const uint32_t baud_options[];
extern int baud_select_cursor;
extern const char* port_names[];
extern const int port_tx_pins[];
extern int port_select_cursor;

// タッチ行判定
extern int row_y0[MAX_EVENTS];
extern int row_y1[MAX_EVENTS];

// 日付ヘッダー
extern int date_header_y0[10];
extern int date_header_y1[10];
extern int date_header_count;

// 表示内容スナップショット
#define MAX_DISPLAY_ROWS 20
#define DISPLAY_TEXT_LEN 256
struct DisplayRow {
    char display_text[DISPLAY_TEXT_LEN];
};
extern DisplayRow last_pushed[MAX_DISPLAY_ROWS];
extern int last_pushed_count;
extern bool row_changed[MAX_DISPLAY_ROWS];

String computeRowDisplayText(int evtIdx);
void saveDisplaySnapshot();
bool displayContentChanged();

// タイミング・同期状態
extern time_t last_fetch;                 // 最後に全件同期が成功した時刻
extern int  sync_fail_count;              // 連続同期失敗
extern unsigned long host_lost_since_ms;  // ホスト不達の起点 (0=正常)
extern bool host_online;                  // 直近のハートビート成功
extern long host_rev;                     // ホストのテーブル版
extern long local_rev;                    // 端末が持つテーブル版
extern int  src_fail_mask;                // ホスト側ICS失敗ビット (ヘッダ表示用)
extern unsigned long hb_interval_ms;
extern int  full_sync_sec;
extern time_t host_next_alarm;
extern bool reboot_pending;
extern unsigned long last_interaction_ms;
extern time_t last_alarm_debug;
extern time_t last_auto_refresh;
extern time_t last_gc16_cleanup;
extern bool time_valid;
extern bool events_cache_dirty;

// MIDI
extern SimpleMIDIPlayer midi;
extern bool midi_playing;

// ファイルシステム状態
extern bool fs_ok;

// ハートビート（生存確認インジケーター）
extern M5EPD_Canvas heartbeat_canvas;
extern bool heartbeat_visible;
extern unsigned long last_heartbeat_ms;

extern int displayed_next_event_idx;
extern int partial_refresh_count;

// スイッチ状態

//==============================================================================
// 関数プロトタイプ
//==============================================================================

// config.cpp
void loadConfig();
void saveConfig();

// logger.cpp — Serial + ホストへ転送するログ (SDは使わない)
void logLine(const char* fmt, ...);
int  logDrain(char** out, int max);   // 未送信ログを取り出す
void uiEventPush(const char* kind, int x, int y, const char* info);
int  uiEventDrain(UiEvent* out, int max);
void pollSerialCommands();

// utf8_utils.cpp
bool isUtf8LeadByte(uint8_t c);
int  utf8CharBytes(uint8_t c);
String utf8Substring(const String& s, int maxWidth);
String normalizeFullWidth(const String& s);
String removeUnsupportedChars(const String& s);
String simplifyHtml(const String& s);

// fs_utils.cpp
void waitEPDReady();
bool initFS();
void scanMidiFiles();
bool saveEventsCache();     // 予定テーブルをLittleFSへ保存 (rev不変かつ非dirtyならスキップ)
int  loadEventsCache();     // 起動時: キャッシュから予定テーブルを復元。件数 or -1
bool rtcLoadTime();         // 起動時: BM8563 RTCから時刻を復元 (VLフラグ確認付き)
size_t fsUsed();
size_t fsTotal();

// network.cpp — WiFi + 素のHTTP/1.1クライアント (SSLなし)
bool connectWiFi();
struct HttpResult { int code; long length; };
bool httpBegin(WiFiClient& c, const char* method, const char* path,
               const char* contentType, size_t bodyLen, HttpResult& r,
               const char* extraHeaders = nullptr);
int  httpReadLine(WiFiClient& c, char* buf, int maxLen, unsigned long timeoutMs);
long httpReadBody(WiFiClient& c, char* buf, long maxLen, long contentLen, unsigned long timeoutMs);
bool httpGetBegin(WiFiClient& c, const char* path, HttpResult& r);
bool httpPostJson(const char* path, const char* body, char* respBuf, size_t respSize, int* codeOut = nullptr);
bool downloadMidi(const String& filename, String& localPath);
bool uploadScreenshot();

// sync_client.cpp — ホスト同期
bool fetchAndUpdate();                       // 全件同期 (GET /api/v1/events)
bool sendHeartbeat(bool force = false);      // Active sensing + コマンド受信 + 時刻合わせ
void ackAlarm(const char* alarm_id, const char* result);
void retryPendingAcks();
bool isAlarmLocallyAcked(const char* alarm_id);
void loadPendingAcks();
void alarmIdOf(const EventItem& e, int slot, char* out, size_t outSize);
void applyTimeFromHost(double hostNow, unsigned long rttMs);
void safeReboot();

// midi_player.cpp
void stopAllNotes();
bool startMidiPlayback(const char* filename);
void stopMidiPlayback();
void updateMidiPlayback();
void finishAlarm();
String getMidiPath(int eventIdx);
bool startCommandPlay(const char* midiName, bool isUrl, int durationSec, int repeat,
                      const char* alarm_id, const char* event_id);

// ui_common.cpp
void drawText(const String& s, int x, int y);
void drawTextBold(const String& s, int x, int y, int level = 2);
void saveScreenshot();
String formatTime(int hour, int minute);
void partialRefreshHeader();
void partialRefreshNextLine();
void drawNextEventMarker(int y, int rowH);
void buildStatusText(char* buf, size_t size);
void showMessage(const char* text, int holdMs);

#define LIST_NEXT_MARK_X   124

// ui_list.cpp
void scrollToToday();
void drawList(bool fast = false, bool skip_push = false, bool highlight_changes = false, bool clean_refresh = false);
void updateListCursor(int old_sel, int new_sel);

// ui_detail.cpp
void drawDetail(int idx, bool fast = false);
void drawPlaying(int idx);
void drawPlayingGeneric(const char* title, const char* sub);

// ui_settings.cpp
void drawSettings(bool fast = false);
void handleSettingsSelect();
void drawMidiSelect();
void drawBaudSelect();
void drawPortSelect();

// ui_keyboard.cpp
void drawKeyboard();
int  getKeyboardHit(int tx, int ty);
void processKeyboardHit(int hit);

// input_handler.cpp
void initSwitchISR();
void checkSwitches();
void handleSwitch(char sw);
void handleTouch(int tx, int ty);
void checkAlarms();
int  findEventById(const char* id);

#endif // GLOBALS_H
