#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <M5EPD.h>
#include <WiFi.h>
#include "SimpleMIDIPlayer.h"
#include "types.h"

//==============================================================================
// グローバル変数 (extern宣言 — 実体は globals.cpp)
//==============================================================================

// ハードウェア・描画
extern M5EPD_Canvas canvas;

// 設定・データ
extern Config config;
extern EventItem events[MAX_EVENTS];
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
extern int play_repeat_remaining;
extern unsigned long play_start_ms;
extern int play_duration_ms;

// キーボード (caps/symbolはui_keyboard.cpp内static)
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

// タイミング
extern time_t last_fetch;
extern int fetch_fail_count;
extern unsigned long last_switch_check;
extern unsigned long last_interaction_ms;
extern time_t last_alarm_debug;
extern time_t last_auto_refresh;
extern unsigned long last_sd_check_ms;

// MIDI
extern SimpleMIDIPlayer midi;
extern bool midi_playing;

// SD状態
extern bool sd_healthy;

// スイッチ状態
extern bool sw_l_prev, sw_r_prev, sw_p_prev;

//==============================================================================
// 関数プロトタイプ
//==============================================================================

// config.cpp
void loadConfig();
void saveConfig();

// utf8_utils.cpp
bool isUtf8LeadByte(uint8_t c);
int  utf8CharBytes(uint8_t c);
String utf8Substring(const String& s, int maxWidth);
String normalizeFullWidth(const String& s);
String removeUnsupportedChars(const String& s);

// sd_utils.cpp
bool checkSDHealth();
void reinitSD();
void scanMidiFiles();

// network.cpp
bool connectWiFi();
void sendNtfyNotification(const String& title, const String& message);
bool downloadMidi(const String& filename, String& localPath);

// midi_player.cpp
void stopAllNotes();
bool startMidiPlayback(const char* filename);
void stopMidiPlayback();
void updateMidiPlayback();
void finishAlarm();
String getMidiPath(int eventIdx);

// ics_parser.cpp
bool parseDT(const String& raw, time_t& out, bool& is_allday);
bool parseAlarmMarker(const String& s_raw, bool is_summary, int& off, bool& found,
                      String& midi_file, bool& midi_is_url,
                      int& duration_sec, int& repeat_count);
void sortEvents();
void trimEventsAroundToday(int maxEvents);
void fetchAndUpdate();

// ui_common.cpp
void drawText(const String& s, int x, int y);
String formatTime(int hour, int minute);
void drawHeader(const char* title);

// ui_list.cpp
void scrollToToday();
void drawList();

// ui_detail.cpp
void drawDetail(int idx);
void drawPlaying(int idx);

// ui_settings.cpp
void drawSettings();
void handleSettingsSelect();
void drawMidiSelect();
void drawBaudSelect();
void drawPortSelect();

// ui_keyboard.cpp
void drawKeyboard();
int  getKeyboardHit(int tx, int ty);
void processKeyboardHit(int hit);

// input_handler.cpp
void checkSwitches();
void handleSwitch(char sw);
void handleTouch(int tx, int ty);
void checkAlarms();

#endif // GLOBALS_H
