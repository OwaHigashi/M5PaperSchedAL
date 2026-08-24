#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

//==============================================================================
// ビルドバージョン (※コード更新時はここを変更)
//==============================================================================
#define BUILD_VERSION "103"     // v103: サイドスイッチを割り込みラッチ化 (ブロッキング中の押下取りこぼし解消)

//==============================================================================
// ピン定義
//==============================================================================
#define SW_L_PIN         37      // Lスイッチ (上/戻る)
#define SW_R_PIN         39      // Rスイッチ (下/進む)
#define SW_P_PIN         38      // Pスイッチ (決定/メニュー)

//==============================================================================
// デフォルト設定値・定数
//==============================================================================
#ifndef DEFAULT_SERVER_HOST
#define DEFAULT_SERVER_HOST     "10.1.1.2"
#endif
#ifndef DEFAULT_SERVER_PORT
#define DEFAULT_SERVER_PORT     8765
#endif
#define DEFAULT_MIDI_BAUD       31250   // UNIT_SYNTH_BAUD相当
#define CONFIG_FILE             "/config.json"      // LittleFS (内蔵フラッシュ)
#define PENDING_ACK_FILE        "/pending_acks.txt" // ホスト未達のアラーム完了通知
#define MIDI_DIR                "/midi"
#define MIDI_DL_DIR             "/midi-dl"
#define FONT_PATH               "/fonts/ipaexg.ttf"
#define TZ_DEFAULT              "JST-9"

#define MAX_EVENTS              300
#define MAX_ALARMS_PER_EVENT    6       // 1イベントあたりの最大アラーム数
#define EVENT_ID_LEN            16
#define ITEMS_PER_PAGE          12

// ── ホスト通信 ──
#define HB_INTERVAL_MS_DEFAULT  5000    // ハートビート(Active Sensing)間隔。ホストから上書き可
#define HB_INTERVAL_PLAYING_MS  15000   // MIDI再生中は間引く(タイミング乱れ防止)
#define FULL_SYNC_SEC_DEFAULT   600     // 最低でもこの間隔で全件再取得
#define HTTP_TIMEOUT_MS         4000    // 1リクエストの応答待ち
#define HOST_LOST_WIFI_RESET_MS (10UL * 60UL * 1000UL)   // ホスト不達10分でWiFi張り直し
#define HOST_LOST_REBOOT_MS     (120UL * 60UL * 1000UL)  // ホスト不達2時間で再起動
#define MAX_PENDING_ACKS        16
#define UI_EVENT_QUEUE          24
#define LOG_QUEUE               24
#define LOG_LINE_LEN            160

// ★ 薄文字対策(リフレッシュ波形)
#define GC16_CLEANUP_MIN        20
#define NIGHT_START_HOUR        0
#define NIGHT_END_HOUR          6

#define BAUD_OPTION_COUNT       3
#define PORT_COUNT              3

//==============================================================================
// 構造体定義
//==============================================================================
struct Config {
    // 端末ローカル設定 (LittleFS /config.json)。予定・アラーム関連の設定はすべてホスト側。
    char wifi_ssid[64];
    char wifi_pass[64];
    char server_host[64];
    int  server_port;
    char api_token[64];
    char midi_file[64];         // ローカル既定MIDI (/midi/alarm.mid)
    uint32_t midi_baud;
    int port_select;            // 0=A, 1=B, 2=C
    // ── ホストから受け取る表示設定 (ヘッダ行で毎回上書き) ──
    bool time_24h;
    bool text_wrap;
    int  play_duration;         // 既定鳴動時間(秒) 0=1曲
    int  play_repeat;
    int  alarm_offset_default;  // 表示用
    char midi_default[64];      // ホスト既定MIDI名 (ホストから取得)
    char tz[32];
};

struct EventItem {
    char id[EVENT_ID_LEN];      // ホストが付ける安定ID
    time_t start;
    char text[4000];            // summary \0 description \0
    char midi_file[64];
    bool midi_is_url;           // true: ホストから取得 (/api/v1/midi/<name>)
    bool has_alarm;
    bool is_allday;
    int play_duration_sec;      // 0=1曲 -1=設定値使用
    int play_repeat;            // -1=設定値使用

    int alarm_count;
    int offset_min[MAX_ALARMS_PER_EVENT];
    time_t alarm_time[MAX_ALARMS_PER_EVENT];
    bool triggered[MAX_ALARMS_PER_EVENT];   // ホストの状態 + ローカルpending ack

    const char* summary() const { return text; }
    const char* description() const { return text + strlen(text) + 1; }
};

struct ButtonArea {
    int x0, y0, x1, y1;
};

// ホストへ送る操作イベント (タッチ/スイッチ/画面遷移)
struct UiEvent {
    uint32_t ms;
    char kind[8];     // "touch","btn","ui","alarm"
    int16_t x, y;
    char info[24];
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
    SET_SYNC_NOW,
    SET_SERVER_HOST,
    SET_SERVER_PORT,
    SET_WIFI_SSID,
    SET_WIFI_PASS,
    SET_MIDI_FILE,
    SET_MIDI_BAUD,
    SET_PORT,
    SET_SOUND_TEST,
    SET_SCREENSHOT,
    SET_SAVE_EXIT,
    SET_COUNT
};

#endif // TYPES_H
