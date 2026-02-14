#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

//==============================================================================
// ビルドバージョン (※コード更新時はここを変更)
//==============================================================================
#define BUILD_VERSION "029"

//==============================================================================
// ピン定義
//==============================================================================
#define SW_L_PIN         37      // Lスイッチ (上/戻る)
#define SW_R_PIN         39      // Rスイッチ (下/進む)
#define SW_P_PIN         38      // Pスイッチ (決定/メニュー)

//==============================================================================
// デフォルト設定値・定数
//==============================================================================
#define DEFAULT_MIDI_BAUD       31250   // UNIT_SYNTH_BAUD相当
#define DEFAULT_ALARM_OFFSET    10      // デフォルト10分前
#define DEFAULT_ICS_POLL_SEC    1800    // 30分ごと更新
#define CONFIG_FILE             "/config.json"
#define MIDI_DIR                "/midi"
#define MIDI_DL_DIR             "/midi-dl"
#define FONT_PATH               "/fonts/ipaexg.ttf"
#define TZ_JST                  "JST-9"

#define MAX_EVENTS              300
#define ITEMS_PER_PAGE          12
#define SD_CHECK_INTERVAL_MS    300000  // 5分
#define MIN_HEAP_FOR_FETCH      60000   // ICSフェッチ前の最低ヒープ(byte)

#define BAUD_OPTION_COUNT       3
#define PORT_COUNT              3

//==============================================================================
// 構造体定義
//==============================================================================
struct Config {
    char wifi_ssid[64];
    char wifi_pass[64];
    char ics_url[256];
    char ics_user[64];
    char ics_pass[64];
    char midi_file[64];
    char midi_url[128];
    char ntfy_topic[64];
    uint32_t midi_baud;
    int alarm_offset_default;
    int port_select;            // 0=A, 1=B, 2=C
    bool time_24h;
    bool text_wrap;
    int ics_poll_min;
    int play_duration;          // デフォルト鳴動時間(秒) 0=1曲
    int play_repeat;
    int max_events;
    int max_desc_bytes;
    int min_free_heap;          // ヒープ残量下限(KB)
};

struct EventItem {
    time_t start;
    time_t alarm_time;
    int offset_min;
    char text[4000];            // summary \0 description \0
    char midi_file[64];
    bool midi_is_url;
    bool has_alarm;
    bool triggered;
    bool is_allday;
    int play_duration_sec;      // 0=1曲 -1=設定値使用
    int play_repeat;            // -1=設定値使用

    // summary = text先頭（最初の\0まで）
    const char* summary() const { return text; }
    // description = summary\0の次から
    const char* description() const { return text + strlen(text) + 1; }
};

struct ButtonArea {
    int x0, y0, x1, y1;
};

//==============================================================================
// 列挙型
//==============================================================================
enum UiState {
    UI_LIST,
    UI_DETAIL,
    UI_PLAYING,
    UI_SETTINGS,
    UI_KEYBOARD,
    UI_MIDI_SELECT,
    UI_BAUD_SELECT,
    UI_PORT_SELECT
};

enum SettingsItem {
    SET_WIFI_SSID,
    SET_WIFI_PASS,
    SET_ICS_URL,
    SET_ICS_USER,
    SET_ICS_PASS,
    SET_MIDI_FILE,
    SET_MIDI_URL,
    SET_MIDI_BAUD,
    SET_PORT,
    SET_ALARM_OFFSET,
    SET_TIME_FORMAT,
    SET_TEXT_WRAP,
    SET_ICS_POLL,
    SET_PLAY_DURATION,
    SET_PLAY_REPEAT,
    SET_NTFY_TOPIC,
    SET_NTFY_TEST,
    SET_ICS_UPDATE,
    SET_SOUND_TEST,
    SET_SAVE_EXIT,
    SET_COUNT
};

#endif // TYPES_H
