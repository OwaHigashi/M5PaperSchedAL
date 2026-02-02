/*******************************************************************************
 * M5Paper ICS Alarm with MIDI (Unit Synth)
 * 
 * Features:
 * - ICS calendar fetch with Basic Auth support
 * - %AL% / %AL=xx% alarm marker detection (case insensitive)
 * - MIDI playback via custom player (SysEx support)
 * - Touch UI: List → Detail → Playing
 * - Settings menu via L/R/P switches
 * - On-screen keyboard for WiFi/ICS configuration
 * 
 * Hardware:
 * - M5Paper v1.1
 * - Unit Synth on Port B (GPIO 33 TX)
 ******************************************************************************/

#include <M5EPD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <time.h>
#include <base64.h>

// Custom MIDI player (standard SD library compatible)
#include "SimpleMIDIPlayer.h"

//==============================================================================
// 関数の前方宣言
//==============================================================================
void drawList();
void drawDetail(int idx);
void drawPlaying(int idx);
void drawSettings();
void drawKeyboard();
void drawMidiSelect();
void drawBaudSelect();
void drawPortSelect();
void handleSwitch(char sw);
void handleSettingsSelect();
void handleTouch(int tx, int ty);
void checkSwitches();
void checkAlarms();
void scrollToToday();
void fetchAndUpdate();
bool connectWiFi();
bool startMidiPlayback(const char* filename);
bool downloadMidi(const String& filename, String& localPath);
String getMidiPath(int eventIdx);
void stopMidiPlayback();
void updateMidiPlayback();
void finishAlarm();

//==============================================================================
// ピン定義
//==============================================================================
// 注意: 実際のTXピンは port_tx_pins[] 配列で管理
// Port A=G25, Port B=G26, Port C=G18 (config.port_selectで選択)

// M5Paper側面スイッチ GPIO
#define SW_L_PIN         37      // Lスイッチ (上/戻る)
#define SW_R_PIN         39      // Rスイッチ (下/進む)
#define SW_P_PIN         38      // Pスイッチ (決定/メニュー)

//==============================================================================
// デフォルト設定値
//==============================================================================
#define DEFAULT_MIDI_BAUD       31250
#define DEFAULT_ALARM_OFFSET    10      // デフォルト10分前
#define DEFAULT_ICS_POLL_SEC    1800    // 30分ごと更新
#define CONFIG_FILE             "/config.json"
#define MIDI_DIR                "/midi"

// フォント
#define FONT_PATH               "/fonts/ipaexg.ttf"

// タイムゾーン
#define TZ_JST                  "JST-9"

// 最大イベント数
#define MAX_EVENTS              100

//==============================================================================
// 構造体定義
//==============================================================================
struct Config {
    char wifi_ssid[64];
    char wifi_pass[64];
    char ics_url[256];
    char ics_user[64];          // Basic Auth user
    char ics_pass[64];          // Basic Auth password
    char midi_file[64];
    char midi_url[128];         // MIDIダウンロード用ベースURL
    uint32_t midi_baud;
    int alarm_offset_default;   // %AL%のみの場合のデフォルト分
    int port_select;            // 0=A, 1=B, 2=C
    bool time_24h;              // true=24時間制, false=12時間制
    bool text_wrap;             // true=折り返し, false=切り詰め
    int ics_poll_min;           // ICS更新頻度（分）
    int play_duration;          // デフォルト鳴動時間(秒) 0=1曲
    int play_repeat;            // デフォルト繰り返し回数
};

struct EventItem {
    time_t start;
    time_t alarm_time;
    int offset_min;
    String summary;
    String description;
    String midi_file;           // 指定されたMIDIファイル名
    bool midi_is_url;           // true=URLからダウンロード, false=SDカード
    bool has_alarm;             // %AL%マーカーがあるか
    bool triggered;             // アラーム発火済みフラグ
    bool is_allday;             // 終日予定フラグ
    int play_duration_sec;      // 鳴動時間(秒) 0=1曲 -1=設定値使用
    int play_repeat;            // 繰り返し回数 -1=設定値使用
};

enum UiState {
    UI_LIST,                    // 予定一覧
    UI_DETAIL,                  // 予定詳細
    UI_PLAYING,                 // アラーム再生中
    UI_SETTINGS,                // 設定メニュー
    UI_KEYBOARD,                // キーボード入力
    UI_MIDI_SELECT,             // MIDIファイル選択
    UI_BAUD_SELECT,             // ボーレート選択
    UI_PORT_SELECT              // ポート選択
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
    SET_ICS_UPDATE,
    SET_SOUND_TEST,
    SET_SAVE_EXIT,
    SET_COUNT
};

//==============================================================================
// グローバル変数
//==============================================================================
M5EPD_Canvas canvas(&M5.EPD);

Config config;
EventItem events[MAX_EVENTS];
int event_count = 0;

UiState ui_state = UI_LIST;
int selected_event = -1;
int page_start = 0;  // ページ表示開始インデックス
int displayed_count = 0;  // 表示された行数
int row_event_idx[MAX_EVENTS];  // 各行に対応するイベントインデックス
int detail_scroll = 0;  // 詳細画面スクロール位置
#define ITEMS_PER_PAGE 12

// ボタン領域
struct ButtonArea {
    int x0, y0, x1, y1;
};
ButtonArea btn_prev, btn_next, btn_today, btn_detail;
int settings_cursor = 0;
int playing_event = -1;
int play_repeat_remaining = 0;  // 残り繰り返し回数
unsigned long play_start_ms = 0; // 再生開始時刻(millis)
int play_duration_ms = 0;       // 再生制限時間(ms) 0=1曲

// キーボード関連
String keyboard_buffer;
int keyboard_target = -1;       // どの設定項目を編集中か
int keyboard_cursor = 0;
bool keyboard_caps = false;

// MIDIファイルリスト
String midi_files[32];
int midi_file_count = 0;
int midi_select_cursor = 0;

// ボーレート選択肢
const uint32_t baud_options[] = {31250, 31520, 38400};
const int baud_option_count = 3;
int baud_select_cursor = 0;

// ポート選択
const char* port_names[] = {"PORT A (G25)", "PORT B (G26)", "PORT C (G18)"};
const int port_tx_pins[] = {25, 26, 18};
int port_select_cursor = 0;

// タッチ行判定用
int row_y0[MAX_EVENTS];
int row_y1[MAX_EVENTS];

// 更新タイミング
time_t last_fetch = 0;
int fetch_fail_count = 0;  // ICS取得連続失敗回数
unsigned long last_switch_check = 0;
unsigned long last_interaction_ms = 0;  // 最後の操作時刻(millis)
time_t last_alarm_debug = 0;            // 最後のアラームデバッグ出力時刻
time_t last_auto_refresh = 0;           // 最後の自動表示更新時刻

// MIDI再生
SimpleMIDIPlayer midi;
bool midi_playing = false;

// スイッチ状態
bool sw_l_prev = true;
bool sw_r_prev = true;
bool sw_p_prev = true;

//==============================================================================
// MIDI コールバック (SysEx含む全送信)
//==============================================================================
void midiSendCallback(uint8_t* data, uint16_t len) {
    if (len > 0) {
        Serial2.write(data, len);
    }
}

void sysexCallback(uint8_t* data, uint32_t len) {
    if (len > 0) {
        Serial2.write(data, len);
    }
}

// Control Change送信
void sendCC(uint8_t ch, uint8_t cc, uint8_t val) {
    uint8_t msg[3] = {(uint8_t)(0xB0 | (ch & 0x0F)), cc, val};
    Serial2.write(msg, 3);
}

// 全チャンネル停止 + GM Reset
void stopAllNotes() {
    // 1. 全チャンネル: All Sound Off + All Notes Off + Reset All Controllers
    for (int ch = 0; ch < 16; ch++) {
        sendCC(ch, 120, 0);  // All Sound Off
        sendCC(ch, 123, 0);  // All Notes Off
        sendCC(ch, 121, 0);  // Reset All Controllers
    }
    delay(50);
    
    // 2. GM System On (音源をリセット)
    uint8_t gmReset[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
    Serial2.write(gmReset, sizeof(gmReset));
    delay(100);
    
    Serial.println("MIDI: All notes off + GM Reset sent");
}

//==============================================================================
// 設定ファイル読み書き
//==============================================================================
void loadConfig() {
    // デフォルト値（初回起動用）
    strcpy(config.wifi_ssid, "your_wifi_ssid");
    strcpy(config.wifi_pass, "your_wifi_password");
    strcpy(config.ics_url, "https://example.com/calendar.ics");
    strcpy(config.ics_user, "");
    strcpy(config.ics_pass, "");
    strcpy(config.midi_file, "/midi/alarm.mid");
    strcpy(config.midi_url, "");
    config.midi_baud = DEFAULT_MIDI_BAUD;
    config.alarm_offset_default = DEFAULT_ALARM_OFFSET;
    config.port_select = 1;  // デフォルトPort B
    config.time_24h = true;  // デフォルト24時間制
    config.text_wrap = false; // デフォルト切り詰め
    config.ics_poll_min = 30;  // 30分ごと
    config.play_duration = 0;  // 0=1曲
    config.play_repeat = 1;    // 1回

    if (!SD.exists(CONFIG_FILE)) {
        Serial.println("Config not found, using defaults");
        return;
    }

    File f = SD.open(CONFIG_FILE, FILE_READ);
    if (!f) return;

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.println("JSON parse error");
        return;
    }

    if (doc["wifi_ssid"]) strlcpy(config.wifi_ssid, doc["wifi_ssid"], sizeof(config.wifi_ssid));
    if (doc["wifi_pass"]) strlcpy(config.wifi_pass, doc["wifi_pass"], sizeof(config.wifi_pass));
    if (doc["ics_url"])   strlcpy(config.ics_url, doc["ics_url"], sizeof(config.ics_url));
    if (doc["ics_user"])  strlcpy(config.ics_user, doc["ics_user"], sizeof(config.ics_user));
    if (doc["ics_pass"])  strlcpy(config.ics_pass, doc["ics_pass"], sizeof(config.ics_pass));
    if (doc["midi_file"]) strlcpy(config.midi_file, doc["midi_file"], sizeof(config.midi_file));
    if (doc["midi_url"])  strlcpy(config.midi_url, doc["midi_url"], sizeof(config.midi_url));
    if (doc["midi_baud"]) config.midi_baud = doc["midi_baud"];
    if (doc["alarm_offset"]) config.alarm_offset_default = doc["alarm_offset"];
    if (doc["port_select"]) config.port_select = doc["port_select"];
    if (doc.containsKey("time_24h")) config.time_24h = doc["time_24h"];
    if (doc.containsKey("text_wrap")) config.text_wrap = doc["text_wrap"];
    if (doc["ics_poll_min"]) config.ics_poll_min = doc["ics_poll_min"];
    
    // ICS更新間隔の下限を5分に制限（メモリフラグメンテーション防止）
    if (config.ics_poll_min < 5) {
        Serial.printf("Config: ics_poll_min=%d is too small, setting to 5\n", config.ics_poll_min);
        config.ics_poll_min = 5;
    }
    if (doc.containsKey("play_duration")) config.play_duration = doc["play_duration"];
    if (doc["play_repeat"]) config.play_repeat = doc["play_repeat"];

    Serial.println("Config loaded");
}

void saveConfig() {
    StaticJsonDocument<1024> doc;
    doc["wifi_ssid"] = config.wifi_ssid;
    doc["wifi_pass"] = config.wifi_pass;
    doc["ics_url"] = config.ics_url;
    doc["ics_user"] = config.ics_user;
    doc["ics_pass"] = config.ics_pass;
    doc["midi_file"] = config.midi_file;
    doc["midi_url"] = config.midi_url;
    doc["midi_baud"] = config.midi_baud;
    doc["alarm_offset"] = config.alarm_offset_default;
    doc["port_select"] = config.port_select;
    doc["time_24h"] = config.time_24h;
    doc["text_wrap"] = config.text_wrap;
    doc["ics_poll_min"] = config.ics_poll_min;
    doc["play_duration"] = config.play_duration;
    doc["play_repeat"] = config.play_repeat;

    File f = SD.open(CONFIG_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("Failed to save config");
        return;
    }
    serializeJson(doc, f);
    f.flush();
    f.close();
    Serial.println("Config saved");
}

//==============================================================================
// MIDIファイル列挙
//==============================================================================
void scanMidiFiles() {
    midi_file_count = 0;
    File dir = SD.open(MIDI_DIR);
    if (!dir) {
        Serial.println("MIDI dir not found");
        return;
    }

    while (midi_file_count < 32) {
        File entry = dir.openNextFile();
        if (!entry) break;
        
        String name = entry.name();
        if (!entry.isDirectory() && 
            (name.endsWith(".mid") || name.endsWith(".MID") ||
             name.endsWith(".midi") || name.endsWith(".MIDI"))) {
            midi_files[midi_file_count] = String(MIDI_DIR) + "/" + name;
            midi_file_count++;
        }
        entry.close();
    }
    dir.close();
    Serial.printf("Found %d MIDI files\n", midi_file_count);
}

//==============================================================================
// UTF-8文字列処理
//==============================================================================
// UTF-8文字の先頭バイトかどうか判定
bool isUtf8LeadByte(uint8_t c) {
    // 継続バイト(10xxxxxx)でなければ先頭バイト
    return (c & 0xC0) != 0x80;
}

// UTF-8文字のバイト数を取得
int utf8CharBytes(uint8_t c) {
    if ((c & 0x80) == 0) return 1;      // ASCII
    if ((c & 0xE0) == 0xC0) return 2;   // 2バイト文字
    if ((c & 0xF0) == 0xE0) return 3;   // 3バイト文字（日本語など）
    if ((c & 0xF8) == 0xF0) return 4;   // 4バイト文字（絵文字など）
    return 1;
}

// UTF-8文字列を指定文字数（表示幅）で安全に切り取る
// 日本語=2幅、ASCII=1幅として計算
String utf8Substring(const String& s, int maxWidth) {
    String result;
    int width = 0;
    int i = 0;
    
    while (i < (int)s.length() && width < maxWidth) {
        uint8_t c = s[i];
        int bytes = utf8CharBytes(c);
        
        // 4バイト文字（絵文字など）はスキップ
        if (bytes == 4) {
            i += bytes;
            continue;
        }
        
        // 文字幅を計算（ASCII=1、それ以外=2）
        int charWidth = (bytes == 1) ? 1 : 2;
        
        if (width + charWidth > maxWidth) break;
        
        // 文字をコピー
        for (int j = 0; j < bytes && (i + j) < (int)s.length(); j++) {
            result += s[i + j];
        }
        
        width += charWidth;
        i += bytes;
    }
    
    return result;
}

// 絵文字などの表示できない文字を除去
String removeUnsupportedChars(const String& s) {
    String result;
    int i = 0;
    
    while (i < (int)s.length()) {
        uint8_t c = s[i];
        int bytes = utf8CharBytes(c);
        
        // 4バイト文字（絵文字など）はスキップ
        if (bytes == 4) {
            i += bytes;
            continue;
        }
        
        // 文字をコピー
        for (int j = 0; j < bytes && (i + j) < (int)s.length(); j++) {
            result += s[i + j];
        }
        i += bytes;
    }
    
    return result;
}

//==============================================================================
// ICS取得・解析
//==============================================================================
String unfoldICS(const String& in) {
    String out;
    out.reserve(in.length());
    for (int i = 0; i < (int)in.length();) {
        if (in[i] == '\r' && i + 1 < (int)in.length() && in[i + 1] == '\n') {
            if (i + 2 < (int)in.length() && (in[i + 2] == ' ' || in[i + 2] == '\t')) {
                i += 3;
                continue;
            }
            out += '\n';
            i += 2;
            continue;
        }
        out += in[i++];
    }
    return out;
}

bool parseDT(const String& raw, time_t& out, bool& is_allday) {
    String s = raw;
    s.trim();
    bool utc = s.endsWith("Z");
    if (utc) s.remove(s.length() - 1);

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    is_allday = false;
    
    if (s.length() == 8) {
        // 終日予定 (YYYYMMDD)
        y  = s.substring(0, 4).toInt();
        mo = s.substring(4, 6).toInt();
        d  = s.substring(6, 8).toInt();
        is_allday = true;
    } else if (s.length() >= 15 && s[8] == 'T') {
        y  = s.substring(0, 4).toInt();
        mo = s.substring(4, 6).toInt();
        d  = s.substring(6, 8).toInt();
        h  = s.substring(9, 11).toInt();
        mi = s.substring(11, 13).toInt();
        se = s.substring(13, 15).toInt();
    } else {
        return false;
    }

    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = se;

    if (!utc) {
        out = mktime(&t);
        return out != (time_t)-1;
    }

    // UTC時刻をJSTに変換（+9時間）
    // mktimeでUTC時刻としてエポック秒を取得し、9時間加算
    char* oldtz = getenv("TZ");
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t u = mktime(&t);
    // 必ずJSTに戻す
    setenv("TZ", TZ_JST, 1);
    tzset();
    out = u;  // time_tはUTCのエポック秒、localtime_rでJST表示される
    return out != (time_t)-1;
}

// %AL% または %AL=xx% を検索 (大文字小文字不問)
// %AL% パーサー
// 書式: %AL%, %AL>10%, %AL<10%, %AL+file%, %AL-file%,
//       %AL@秒%, %AL@%(1曲), %AL*回数%
//       組み合わせ自由: %AL>10+file@15*3%
// > = 分前, < = 分後, + = URLからDL, - = SDカードから
// @ = 鳴動秒(@のみ=1曲), * = 繰り返し回数
bool parseAL(const String& s, int& off, bool& found, String& midi_file, 
             bool& midi_is_url, int& duration_sec, int& repeat_count) {
    String lower = s;
    lower.toLowerCase();
    
    found = false;
    off = config.alarm_offset_default;
    midi_file = "";
    midi_is_url = false;
    duration_sec = -1;   // -1 = 設定値使用
    repeat_count = -1;   // -1 = 設定値使用
    int maxOff = -1;
    
    int searchStart = 0;
    while (searchStart < (int)lower.length()) {
        int p = lower.indexOf("%al", searchStart);
        if (p < 0) break;
        
        // 終了の%を探す
        int endpct = lower.indexOf('%', p + 3);
        if (endpct < 0) {
            searchStart = p + 3;
            continue;
        }
        
        found = true;
        String content = s.substring(p + 3, endpct);  // %AL と % の間
        
        int thisOff = config.alarm_offset_default;
        bool thisIsUrl = false;
        String thisFile = "";
        
        // パース: >数値, <数値, +ファイル, -ファイル, @秒, *回数
        int i = 0;
        while (i < (int)content.length()) {
            char c = content[i];
            
            if (c == '>' || c == '<' || c == '=') {
                // 時間オフセットを読む
                int numStart = i + 1;
                int numEnd = numStart;
                while (numEnd < (int)content.length() && 
                       (isdigit(content[numEnd]) || content[numEnd] == ' ')) {
                    numEnd++;
                }
                if (numEnd > numStart) {
                    String num = content.substring(numStart, numEnd);
                    num.trim();
                    int val = num.toInt();
                    if (val >= 0 && val <= 24 * 60) {
                        thisOff = (c == '<') ? -val : val;
                    }
                }
                i = numEnd;
            } else if (c == '+' || c == '-') {
                // ファイル名を読む
                thisIsUrl = (c == '+');
                int fileStart = i + 1;
                int fileEnd = fileStart;
                while (fileEnd < (int)content.length() && 
                       content[fileEnd] != '>' && content[fileEnd] != '<' &&
                       content[fileEnd] != '=' && content[fileEnd] != '@' &&
                       content[fileEnd] != '*') {
                    fileEnd++;
                }
                if (fileEnd > fileStart) {
                    thisFile = content.substring(fileStart, fileEnd);
                    thisFile.trim();
                }
                i = fileEnd;
            } else if (c == '@') {
                // 鳴動時間（秒）
                int numStart = i + 1;
                int numEnd = numStart;
                while (numEnd < (int)content.length() && 
                       (isdigit(content[numEnd]) || content[numEnd] == ' ')) {
                    numEnd++;
                }
                if (numEnd > numStart) {
                    String num = content.substring(numStart, numEnd);
                    num.trim();
                    duration_sec = num.toInt();  // 秒指定（制約なし）
                } else {
                    duration_sec = 0;  // @のみ = 1曲
                }
                i = numEnd;
            } else if (c == '*') {
                // 繰り返し回数
                int numStart = i + 1;
                int numEnd = numStart;
                while (numEnd < (int)content.length() && 
                       (isdigit(content[numEnd]) || content[numEnd] == ' ')) {
                    numEnd++;
                }
                if (numEnd > numStart) {
                    String num = content.substring(numStart, numEnd);
                    num.trim();
                    repeat_count = num.toInt();  // 回数（制約なし）
                }
                i = numEnd;
            } else {
                i++;
            }
        }
        
        // 最大オフセットを採用
        if (abs(thisOff) > abs(maxOff) || maxOff == -1) {
            maxOff = thisOff;
        }
        
        // ファイル指定があれば記録
        if (thisFile.length() > 0) {
            midi_file = thisFile;
            midi_is_url = thisIsUrl;
        }
        
        searchStart = endpct + 1;
    }
    
    if (found && maxOff != -1) {
        off = maxOff;
    }
    
    return found;
}

void sortEvents() {
    // 開始時刻でソート
    for (int i = 0; i < event_count; i++) {
        for (int j = i + 1; j < event_count; j++) {
            if (events[j].start < events[i].start) {
                EventItem tmp = events[i];
                events[i] = events[j];
                events[j] = tmp;
            }
        }
    }
}

String httpGetICS() {
    if (strlen(config.ics_url) == 0) return "";
    
    // WiFi接続確認
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, skipping ICS fetch");
        return "";
    }

    WiFiClientSecure client;
    client.setInsecure();  // 証明書検証スキップ
    client.setTimeout(15);  // 15秒タイムアウト
    
    HTTPClient http;
    http.setTimeout(15000);  // 15秒タイムアウト
    http.setConnectTimeout(10000);  // 10秒接続タイムアウト
    
    if (!http.begin(client, config.ics_url)) {
        Serial.println("HTTP begin failed");
        return "";
    }

    // Basic認証
    if (strlen(config.ics_user) > 0) {
        String auth = String(config.ics_user) + ":" + String(config.ics_pass);
        String authHeader = "Basic " + base64::encode(auth);
        http.addHeader("Authorization", authHeader);
    }

    int code = http.GET();
    if (code <= 0) {
        Serial.printf("HTTP GET failed: %d (WiFi:%d)\n", code, WiFi.status());
        http.end();
        return "";
    }
    
    String body = http.getString();
    http.end();
    return body;
}

void parseICS(const String& unfolded) {
    time_t now = time(nullptr);
    
    // 古いイベントのStringをクリア（メモリ解放）
    for (int i = 0; i < event_count; i++) {
        events[i].summary = "";
        events[i].description = "";
        events[i].midi_file = "";
    }
    event_count = 0;

    bool inEvent = false;
    String dtstart_raw, summary, desc;

    int pos = 0;
    while (pos < (int)unfolded.length()) {
        int nl = unfolded.indexOf('\n', pos);
        if (nl < 0) nl = unfolded.length();
        String line = unfolded.substring(pos, nl);
        pos = nl + 1;
        line.trim();
        if (!line.length()) continue;

        if (line == "BEGIN:VEVENT") {
            inEvent = true;
            dtstart_raw = "";
            summary = "";
            desc = "";
            continue;
        }
        if (line == "END:VEVENT") {
            if (inEvent && event_count < MAX_EVENTS - 1) {  // 1枠テストアラーム用に確保
                time_t st = 0;
                bool is_allday = false;
                if (parseDT(dtstart_raw, st, is_allday)) {
                    // 過去24時間 〜 将来1年以内の予定を表示
                    if (st > now - 86400 && st < now + 365 * 86400) {
                        // %AL%をタイトル、説明、どこでも検索
                        String all_text = summary + " " + desc;
                        int off = 0;
                        bool hasAL = false;
                        String midi_file = "";
                        bool midi_is_url = false;
                        int ev_duration = -1;
                        int ev_repeat = -1;
                        parseAL(all_text, off, hasAL, midi_file, midi_is_url,
                                ev_duration, ev_repeat);
                        
                        events[event_count].start = st;
                        events[event_count].summary = summary;
                        events[event_count].description = desc;
                        events[event_count].has_alarm = hasAL;
                        events[event_count].is_allday = is_allday;
                        events[event_count].midi_file = midi_file;
                        events[event_count].midi_is_url = midi_is_url;
                        events[event_count].play_duration_sec = ev_duration;
                        events[event_count].play_repeat = ev_repeat;
                        
                        if (hasAL) {
                            events[event_count].offset_min = off;
                            // off < 0 の場合は予定後にアラーム
                            events[event_count].alarm_time = st - (time_t)off * 60;
                            events[event_count].triggered = (events[event_count].alarm_time < now);
                        } else {
                            events[event_count].offset_min = 0;
                            events[event_count].alarm_time = 0;
                            events[event_count].triggered = false;
                        }
                        event_count++;
                    }
                }
            }
            inEvent = false;
            continue;
        }
        if (!inEvent) continue;

        if (line.startsWith("DTSTART")) {
            int c = line.indexOf(':');
            if (c > 0) dtstart_raw = line.substring(c + 1);
        } else if (line.startsWith("SUMMARY")) {
            int c = line.indexOf(':');
            if (c > 0) summary = line.substring(c + 1);
        } else if (line.startsWith("DESCRIPTION")) {
            int c = line.indexOf(':');
            if (c > 0) desc = line.substring(c + 1);
        }
    }

    sortEvents();
}

void fetchAndUpdate() {
    Serial.printf("Fetching ICS... (heap: %d)\n", ESP.getFreeHeap());
    
    {
        // ブロックスコープでraw, unfoldedを確実に解放
        String raw = httpGetICS();
        if (!raw.length()) {
            Serial.printf("ICS fetch failed (fail count: %d)\n", fetch_fail_count + 1);
            last_fetch = time(nullptr);
            fetch_fail_count++;
            return;
        }
        String unfolded = unfoldICS(raw);
        raw = "";  // 即座に解放
        parseICS(unfolded);
        // unfoldedはスコープ終了で解放
    }
    
    last_fetch = time(nullptr);
    fetch_fail_count = 0;
    Serial.printf("Fetched %d events (heap: %d, next fetch in %d min)\n", 
                  event_count, ESP.getFreeHeap(), config.ics_poll_min);
}

//==============================================================================
// MIDI ダウンロードと再生
//==============================================================================
#define MIDI_DL_DIR "/midi-dl"

// URLからMIDIファイルをダウンロード
bool downloadMidi(const String& filename, String& localPath) {
    if (strlen(config.midi_url) == 0) {
        Serial.println("midi_url not configured");
        return false;
    }
    
    // ダウンロードフォルダ作成
    if (!SD.exists(MIDI_DL_DIR)) {
        SD.mkdir(MIDI_DL_DIR);
    }
    
    localPath = String(MIDI_DL_DIR) + "/" + filename;
    
    // すでにダウンロード済みならスキップ
    if (SD.exists(localPath.c_str())) {
        Serial.printf("MIDI already downloaded: %s\n", localPath.c_str());
        return true;
    }
    
    // URLを構築
    String url = String(config.midi_url);
    if (!url.endsWith("/")) url += "/";
    url += filename;
    
    Serial.printf("Downloading MIDI: %s\n", url.c_str());
    
    HTTPClient http;
    http.begin(url);
    int code = http.GET();
    
    if (code != HTTP_CODE_OK) {
        Serial.printf("MIDI download failed: %d\n", code);
        http.end();
        return false;
    }
    
    File f = SD.open(localPath.c_str(), FILE_WRITE);
    if (!f) {
        Serial.println("Failed to create MIDI file");
        http.end();
        return false;
    }
    
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[512];
    while (http.connected() && stream->available()) {
        int len = stream->read(buf, sizeof(buf));
        if (len > 0) {
            f.write(buf, len);
        }
    }
    f.flush();
    f.close();
    http.end();
    
    Serial.printf("MIDI downloaded: %s\n", localPath.c_str());
    return true;
}

// イベント用のMIDIファイルパスを取得
String getMidiPath(int eventIdx) {
    if (eventIdx < 0 || eventIdx >= event_count) {
        return config.midi_file;
    }
    
    EventItem& e = events[eventIdx];
    
    if (e.midi_file.length() == 0) {
        // デフォルトファイル
        return config.midi_file;
    }
    
    if (e.midi_is_url) {
        // URLからダウンロード
        String localPath;
        if (downloadMidi(e.midi_file, localPath)) {
            return localPath;
        }
        // ダウンロード失敗時はデフォルト
        return config.midi_file;
    } else {
        // SDカードのmidiフォルダから
        return String("/midi/") + e.midi_file;
    }
}

bool startMidiPlayback(const char* filename) {
    if (!SD.exists(filename)) {
        Serial.printf("MIDI file not found: %s\n", filename);
        return false;
    }

    if (!midi.load(filename)) {
        Serial.println("MIDI load failed");
        return false;
    }

    midi.setMidiCallback(midiSendCallback);
    midi.setSysExCallback(sysexCallback);
    midi.play();
    midi_playing = true;
    
    Serial.printf("MIDI playback started: %s\n", filename);
    return true;
}

void stopMidiPlayback() {
    if (midi_playing) {
        midi.stop();
        midi.close();
        stopAllNotes();
        midi_playing = false;
        Serial.println("MIDI playback stopped");
    }
}

// アラーム再生終了処理
void finishAlarm() {
    stopMidiPlayback();
    if (playing_event >= 0 && playing_event < event_count) {
        events[playing_event].triggered = true;
    }
    playing_event = -1;
    play_repeat_remaining = 0;
    play_duration_ms = 0;
    ui_state = UI_LIST;
    scrollToToday();
    drawList();
}

void updateMidiPlayback() {
    if (!midi_playing) return;

    // 鳴動時間チェック（0=1曲なのでスキップ）
    if (play_duration_ms > 0 && (millis() - play_start_ms) >= (unsigned long)play_duration_ms) {
        Serial.println("Play duration reached");
        finishAlarm();
        return;
    }

    if (!midi.update()) {
        // 1回の再生完了
        play_repeat_remaining--;
        Serial.printf("Play finished, remaining: %d\n", play_repeat_remaining);
        
        if (play_repeat_remaining > 0) {
            // 鳴動時間内であれば繰り返し
            if (play_duration_ms > 0 && (millis() - play_start_ms) >= (unsigned long)play_duration_ms) {
                finishAlarm();
                return;
            }
            // リピート再生
            stopMidiPlayback();
            String midiPath = getMidiPath(playing_event);
            if (!startMidiPlayback(midiPath.c_str())) {
                finishAlarm();
            }
        } else {
            // 全回数再生完了
            if (ui_state == UI_PLAYING) {
                finishAlarm();
            }
        }
    }
}

//==============================================================================
// WiFi接続
//==============================================================================
bool connectWiFi() {
    if (strlen(config.wifi_ssid) == 0) return false;

    // 既存の接続を切断してリセット
    WiFi.disconnect(true);
    delay(100);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifi_ssid, config.wifi_pass);
    
    Serial.print("Connecting to WiFi");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected: " + WiFi.localIP().toString());
        Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
        return true;
    }
    Serial.println("WiFi connection failed");
    return false;
}

// 時刻フォーマット
String formatTime(int hour, int minute) {
    char buf[16];
    if (config.time_24h) {
        snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    } else {
        const char* ampm = (hour < 12) ? "AM" : "PM";
        int h12 = hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, sizeof(buf), "%2d:%02d%s", h12, minute, ampm);
    }
    return String(buf);
}

//==============================================================================
// 描画関数
//==============================================================================
// E-Inkでは重ね書きは逆効果なので、シンプルに描画
void drawText(const String& s, int x, int y) {
    canvas.drawString(s, x, y);
}

void drawHeader(const char* title) {
    canvas.setTextSize(24);
    canvas.setTextColor(15);  // 黒
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d  %s",
             lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, title);
    drawText(buf, 10, 10);
}

// 日付行インデックス（タップ不可の行）
int date_header_y0[10];
int date_header_y1[10];
int date_header_count = 0;

// 今日の先頭にスクロール
void scrollToToday() {
    time_t now_t = time(nullptr);
    struct tm now_tm;
    localtime_r(&now_t, &now_tm);
    int today = now_tm.tm_mday + now_tm.tm_mon * 100 + now_tm.tm_year * 10000;
    
    for (int i = 0; i < event_count; i++) {
        struct tm t;
        localtime_r(&events[i].start, &t);
        int day = t.tm_mday + t.tm_mon * 100 + t.tm_year * 10000;
        if (day >= today) {
            page_start = i;
            selected_event = i;
            return;
        }
    }
    // 今日以降の予定がなければ先頭
    page_start = 0;
    selected_event = 0;
}

void drawList() {
    canvas.fillCanvas(0);
    canvas.setTextColor(15);
    canvas.setTextDatum(TL_DATUM);
    
    // ヘッダー
    canvas.setTextSize(26);
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[64];
    String timeNow = formatTime(lt.tm_hour, lt.tm_min);
    snprintf(buf, sizeof(buf), "%02d/%02d %s", lt.tm_mon + 1, lt.tm_mday, timeNow.c_str());
    drawText(buf, 10, 8);
    
    // WiFi状態
    canvas.setTextSize(22);
    String status = (WiFi.status() == WL_CONNECTED) ? "WiFi:OK" : "WiFi:NG";
    drawText(status, 430, 10);

    // 一覧表示（画面は540x960）
    int y = 45;
    int rowH = 60;
    int lastDay = -1;
    date_header_count = 0;
    int displayed = 0;
    int listBottom = 850;  // ボタン領域の上まで

    for (int i = page_start; i < event_count && y < listBottom; i++) {
        struct tm st;
        localtime_r(&events[i].start, &st);
        
        // 日付が変わったら日付ヘッダーを挿入
        int thisDay = st.tm_mday + st.tm_mon * 100 + st.tm_year * 10000;
        if (thisDay != lastDay && date_header_count < 10) {
            if (y + 34 + rowH > listBottom) break;
            
            // 日付ヘッダー行（グレー背景）
            canvas.fillRect(0, y, 540, 34, 6);
            canvas.setTextSize(24);
            snprintf(buf, sizeof(buf), "── %d/%d (%s) ──", 
                     st.tm_mon + 1, st.tm_mday,
                     (st.tm_wday == 0) ? "日" : (st.tm_wday == 1) ? "月" :
                     (st.tm_wday == 2) ? "火" : (st.tm_wday == 3) ? "水" :
                     (st.tm_wday == 4) ? "木" : (st.tm_wday == 5) ? "金" : "土");
            drawText(buf, 130, y + 4);
            date_header_y0[date_header_count] = y;
            date_header_y1[date_header_count] = y + 34;
            date_header_count++;
            y += 38;
            lastDay = thisDay;
        }
        
        if (y + rowH > listBottom) break;

        row_y0[displayed] = y;
        row_y1[displayed] = y + rowH;
        row_event_idx[displayed] = i;  // この行の実際のイベントインデックス

        // 背景（選択時）
        if (i == selected_event) {
            canvas.fillRect(0, y, 540, rowH, 3);
        }

        canvas.setTextSize(26);
        
        // 時刻 or 終日
        String timeStr;
        if (events[i].is_allday) {
            timeStr = "[終日]";
        } else {
            timeStr = formatTime(st.tm_hour, st.tm_min);
        }
        
        // アラームマーク
        String alarmMark = "";
        if (events[i].has_alarm) {
            alarmMark = events[i].triggered ? "*" : "♪";
        }
        
        // サマリー（絵文字除去してUTF-8安全に切り取り）
        String summary = removeUnsupportedChars(events[i].summary);
        // 表示幅: 時刻7幅 + マーク2幅 = 9幅使用、残り約28幅
        int maxWidth = config.text_wrap ? 28 : 32;
        String dispSummary = utf8Substring(summary, maxWidth);
        
        if (config.text_wrap && summary.length() > dispSummary.length()) {
            // 折り返し：2行表示
            String line1 = timeStr + " " + alarmMark + dispSummary;
            drawText(line1, 10, y + 3);
            canvas.setTextSize(22);
            // 2行目
            String rest = summary.substring(dispSummary.length());
            String line2 = utf8Substring(rest, 34);
            drawText(line2, 85, y + 32);
        } else {
            String line = timeStr + " " + alarmMark + dispSummary;
            if (summary.length() > dispSummary.length()) {
                line += "..";
            }
            drawText(line, 10, y + 16);
        }
        
        displayed++;
        y += rowH;
    }
    displayed_count = displayed;  // 表示行数を記録

    // 次のアラーム表示
    canvas.setTextSize(22);
    int nextIdx = -1;
    for (int i = 0; i < event_count; i++) {
        if (events[i].has_alarm && !events[i].triggered && events[i].alarm_time > now) {
            nextIdx = i;
            break;
        }
    }
    if (nextIdx >= 0) {
        struct tm al;
        localtime_r(&events[nextIdx].alarm_time, &al);
        String alTime = formatTime(al.tm_hour, al.tm_min);
        snprintf(buf, sizeof(buf), "次AL:%s", alTime.c_str());
        drawText(buf, 380, 860);
    }

    // ページ情報
    canvas.setTextSize(20);
    snprintf(buf, sizeof(buf), "%d/%d件", selected_event + 1, event_count);
    drawText(buf, 10, 860);

    // ボタン描画（画面下部 y=900付近）
    int btnY = 900;
    int btnH = 50;
    canvas.setTextSize(22);
    
    // 前日ボタン
    btn_prev = {5, btnY, 125, btnY + btnH};
    canvas.drawRect(btn_prev.x0, btn_prev.y0, 120, btnH, 15);
    canvas.drawString("<前日", 30, btnY + 14);
    
    // 翌日ボタン
    btn_next = {130, btnY, 255, btnY + btnH};
    canvas.drawRect(btn_next.x0, btn_next.y0, 125, btnH, 15);
    canvas.drawString("翌日>", 160, btnY + 14);
    
    // 今日ボタン
    btn_today = {260, btnY, 385, btnY + btnH};
    canvas.drawRect(btn_today.x0, btn_today.y0, 125, btnH, 15);
    canvas.drawString("今日", 295, btnY + 14);
    
    // 詳細ボタン
    btn_detail = {390, btnY, 535, btnY + btnH};
    canvas.drawRect(btn_detail.x0, btn_detail.y0, 145, btnH, 15);
    canvas.drawString("詳細", 435, btnY + 14);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawDetail(int idx) {
    if (idx < 0 || idx >= event_count) return;

    canvas.fillCanvas(0);  // 白
    canvas.setTextColor(15);  // 黒
    canvas.setTextDatum(TL_DATUM);

    EventItem& e = events[idx];
    struct tm st;
    localtime_r(&e.start, &st);

    // ヘッダー（日付と時刻、終日対応）
    canvas.setTextSize(28);
    char buf[128];
    if (e.is_allday) {
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d [終日]",
                 st.tm_year + 1900, st.tm_mon + 1, st.tm_mday);
    } else {
        String timeStr = formatTime(st.tm_hour, st.tm_min);
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d %s",
                 st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, timeStr.c_str());
    }
    drawText(buf, 10, 10);

    // アラーム情報
    canvas.setTextSize(22);
    if (e.has_alarm) {
        struct tm al;
        localtime_r(&e.alarm_time, &al);
        String alTime = formatTime(al.tm_hour, al.tm_min);
        String offsetStr;
        if (e.offset_min > 0) {
            offsetStr = String(e.offset_min) + "分前";
        } else if (e.offset_min < 0) {
            offsetStr = String(-e.offset_min) + "分後";
        } else {
            offsetStr = "時刻通り";
        }
        snprintf(buf, sizeof(buf), "アラーム: %s (%s) %s",
                 alTime.c_str(), offsetStr.c_str(), 
                 e.triggered ? "[済]" : "");
        drawText(buf, 10, 45);
        
        // 追加情報行
        canvas.setTextSize(18);
        String extraInfo = "";
        
        // MIDIファイル指定
        if (e.midi_file.length() > 0) {
            extraInfo += e.midi_is_url ? "♪URL:" : "♪SD:";
            extraInfo += e.midi_file;
        }
        
        // 鳴動時間
        int dur = e.play_duration_sec >= 0 ? e.play_duration_sec : config.play_duration;
        if (extraInfo.length() > 0) extraInfo += " ";
        extraInfo += dur == 0 ? "1曲" : String(dur) + "秒";
        
        // 繰り返し
        int rep = e.play_repeat >= 0 ? e.play_repeat : config.play_repeat;
        extraInfo += " x" + String(rep);
        
        drawText(extraInfo, 10, 72);
    } else {
        drawText("アラーム: なし", 10, 45);
    }

    // SUMMARY（複数行対応、UTF-8安全）
    canvas.setTextSize(28);
    int y = 100;
    String summary = removeUnsupportedChars(e.summary);
    int summaryWidth = 36;  // 表示幅（日本語18文字、ASCII36文字）
    while (summary.length() > 0 && y < 200) {
        String line = utf8Substring(summary, summaryWidth);
        drawText(line, 10, y);
        summary = summary.substring(line.length());
        y += 36;
    }

    // DESCRIPTION (折り返し表示、スクロール対応、UTF-8安全)
    canvas.setTextSize(22);
    y = max(y + 10, 210);
    int maxY = 890;  // フッターの上まで
    String desc = removeUnsupportedChars(e.description);
    
    // スクロール位置分スキップ
    int skipLines = detail_scroll;
    int lineHeight = 28;
    int lineWidth = 42;  // 表示幅（日本語21文字、ASCII42文字）
    
    while (desc.length() > 0) {
        String line = utf8Substring(desc, lineWidth);
        if (skipLines > 0) {
            skipLines--;
        } else if (y < maxY) {
            drawText(line, 10, y);
            y += lineHeight;
        }
        if (line.length() == 0) break;  // 無限ループ防止
        desc = desc.substring(line.length());
    }

    // フッター
    canvas.setTextSize(22);
    drawText("タップ:戻る  L/R:スクロール", 10, 910);
    
    // スクロール位置表示
    if (detail_scroll > 0) {
        drawText("↑", 500, 200);
    }
    if (y >= maxY && desc.length() > 0) {
        drawText("↓", 500, 850);
    }

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawPlaying(int idx) {
    if (idx < 0 || idx >= event_count) return;

    canvas.fillCanvas(0);  // 白
    canvas.setTextColor(15);  // 黒
    canvas.setTextDatum(MC_DATUM);

    EventItem& e = events[idx];

    // 中央に大きく表示
    canvas.setTextSize(48);
    canvas.drawString("ALARM!", 270, 60);

    canvas.setTextSize(32);
    // サマリー（絵文字除去、UTF-8安全に切り取り）
    String summary = removeUnsupportedChars(e.summary);
    String line1 = utf8Substring(summary, 30);
    canvas.drawString(line1, 270, 140);
    if (summary.length() > line1.length()) {
        String rest = summary.substring(line1.length());
        String line2 = utf8Substring(rest, 30);
        canvas.drawString(line2, 270, 178);
    }

    struct tm st;
    localtime_r(&e.start, &st);
    String timeStr = formatTime(st.tm_hour, st.tm_min);
    canvas.setTextSize(56);
    canvas.drawString(timeStr, 270, 250);

    // 鳴動情報
    canvas.setTextSize(22);
    String info = "";
    if (play_duration_ms > 0) {
        info += String(play_duration_ms / 1000) + "秒";
    } else {
        info += "1曲";
    }
    info += " x" + String(play_repeat_remaining) + "回";
    canvas.drawString(info, 270, 320);

    // DESCRIPTION表示（左揃え、複数行）
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(24);
    String desc = removeUnsupportedChars(e.description);
    int y = 370;
    int maxY = 880;
    int lineWidth = 36;  // 表示幅（日本語18文字、ASCII36文字）
    int lineHeight = 32;
    while (desc.length() > 0 && y < maxY) {
        String dline = utf8Substring(desc, lineWidth);
        if (dline.length() == 0) break;
        canvas.drawString(dline, 20, y);
        desc = desc.substring(dline.length());
        y += lineHeight;
    }

    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(28);
    canvas.drawString("タップで停止", 270, 920);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawSettings() {
    Serial.printf("drawSettings() called (cursor=%d, heap=%d)\n", settings_cursor, ESP.getFreeHeap());
    canvas.fillCanvas(0);  // 白
    canvas.setTextColor(15);  // 黒
    canvas.setTextDatum(TL_DATUM);
    
    // ヘッダー
    canvas.setTextSize(26);
    drawText("=== 設定 ===", 10, 8);

    canvas.setTextSize(22);
    int y = 45;
    int rowH = 50;

    static const char* labels[] = {
        "WiFi SSID",
        "WiFi Pass",
        "ICS URL",
        "ICS User",
        "ICS Pass",
        "MIDI File",
        "MIDI URL",
        "MIDI Baud",
        "Port",
        "Alarm Offset",
        "Time Format",
        "Text Display",
        "ICS Poll",
        "Play Duration",
        "Play Repeat",
        "ICS Update",
        "Sound Test",
        "Save & Exit"
    };

    static const char* dur_labels[] = {"1曲", "5秒", "10秒", "15秒", "20秒"};
    static const int dur_values[] = {0, 5, 10, 15, 20};
    static const int dur_count = 5;

    // settings_cursorを先頭にして描画（スクロールビュー）
    int maxVisible = (895 - y) / rowH;  // ナビボタン分を除く

    for (int n = 0; n < maxVisible; n++) {
        int i = settings_cursor + n;
        if (i >= SET_COUNT) break;

        // 先頭項目（選択中）はグレー背景
        if (n == 0) {
            canvas.fillRect(0, y, 540, rowH, 4);
        }

        // 値を取得
        canvas.setTextSize(22);
        String val;
        switch (i) {
            case SET_WIFI_SSID: val = config.wifi_ssid; break;
            case SET_WIFI_PASS: val = strlen(config.wifi_pass) > 0 ? "****" : "(empty)"; break;
            case SET_ICS_URL:   val = config.ics_url; break;
            case SET_ICS_USER:  val = strlen(config.ics_user) > 0 ? config.ics_user : "(empty)"; break;
            case SET_ICS_PASS:  val = strlen(config.ics_pass) > 0 ? "****" : "(empty)"; break;
            case SET_MIDI_FILE: val = config.midi_file; break;
            case SET_MIDI_URL:  val = strlen(config.midi_url) > 0 ? config.midi_url : "(empty)"; break;
            case SET_MIDI_BAUD: val = String(config.midi_baud); break;
            case SET_PORT:      val = port_names[config.port_select]; break;
            case SET_ALARM_OFFSET: val = String(config.alarm_offset_default) + "分"; break;
            case SET_TIME_FORMAT: val = config.time_24h ? "24h" : "12h"; break;
            case SET_TEXT_WRAP: val = config.text_wrap ? "折り返し" : "切り詰め"; break;
            case SET_ICS_POLL: val = String(config.ics_poll_min) + "分"; break;
            case SET_PLAY_DURATION: {
                bool found = false;
                for (int d = 0; d < dur_count; d++) {
                    if (dur_values[d] == config.play_duration) {
                        val = dur_labels[d]; found = true; break;
                    }
                }
                if (!found) val = String(config.play_duration) + "秒";
                break;
            }
            case SET_PLAY_REPEAT: val = String(config.play_repeat) + "回"; break;
            case SET_ICS_UPDATE: val = "[実行]"; break;
            case SET_SOUND_TEST: val = "[実行]"; break;
            case SET_SAVE_EXIT:  val = "[実行]"; break;
        }

        // ラベル行
        drawText(String(labels[i]) + ":", 10, y + 3);
        // 値行
        drawText(val, 10, y + 26);

        y += rowH;
    }

    // ナビゲーションボタン (画面下部)
    int navY = 900;
    int navH = 48;
    canvas.setTextSize(22);

    // 先頭ボタン
    canvas.drawRect(5, navY, 130, navH, 15);
    canvas.drawString("<<先頭", 25, navY + 12);

    // 末尾ボタン
    canvas.drawRect(145, navY, 130, navH, 15);
    canvas.drawString("末尾>>", 165, navY + 12);

    // 戻るボタン
    canvas.drawRect(285, navY, 130, navH, 15);
    canvas.drawString("戻る", 320, navY + 12);

    // 位置表示
    canvas.setTextSize(20);
    char footer[32];
    snprintf(footer, sizeof(footer), "[%d/%d]", settings_cursor + 1, SET_COUNT);
    drawText(footer, 440, navY + 14);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

//==============================================================================
// キーボード描画
//==============================================================================
const char* kb_rows[] = {
    "1234567890-=",
    "qwertyuiop[]",
    "asdfghjkl;'",
    "zxcvbnm,./\\",
    " "
};
const int kb_row_count = 5;

// 記号キーボード
const char* kb_symbol_rows[] = {
    "!@#$%^&*()_+",
    "{}|:\"<>?~`",
    "[]\\;',./",
    " "
};

bool keyboard_symbol_mode = false;

void drawKeyboard() {
    canvas.fillCanvas(0);  // 白 - 960px全体クリア
    canvas.setTextColor(15);  // 黒
    canvas.setTextDatum(TL_DATUM);

    // ========== 上半分: 入力情報エリア (y: 0〜449) ==========
    
    // 編集対象フィールド名
    canvas.setTextSize(26);
    static const char* field_names[] = {
        "WiFi SSID", "WiFi Pass", "ICS URL", "ICS User", "ICS Pass", "", "MIDI URL"
    };
    if (keyboard_target >= 0 && keyboard_target <= SET_MIDI_URL) {
        drawText(String("編集: ") + field_names[keyboard_target], 10, 10);
    }

    // 文字数表示
    canvas.setTextSize(20);
    char lenBuf[16];
    snprintf(lenBuf, sizeof(lenBuf), "%d文字", (int)keyboard_buffer.length());
    drawText(lenBuf, 450, 14);

    // 区切り線
    canvas.drawLine(0, 48, 540, 48, 12);

    // 現在の入力値（大きめに表示、複数行対応）
    canvas.setTextSize(26);
    String display = keyboard_buffer;
    int y = 62;
    int charsPerLine = 26;
    int maxLines = 12;
    int lineCount = 0;
    while (display.length() > 0 && lineCount < maxLines) {
        int len = min((int)display.length(), charsPerLine);
        drawText(display.substring(0, len), 15, y);
        display = display.substring(len);
        y += 30;
        lineCount++;
    }
    // カーソル
    if (lineCount == 0) y = 62;
    drawText("_", 15, y);

    // ========== 下半分: キーボード (y: 460〜960) ==========
    // 500px使える: キー5行*62px=310, gap15, ボタン52px, gap, フッター
    
    canvas.drawLine(0, 455, 540, 455, 12);

    canvas.setTextSize(28);
    int startY = 470;
    int keyW = 42;
    int keyH = 60;
    int spacing = 3;

    const char** rows = keyboard_symbol_mode ? kb_symbol_rows : kb_rows;
    int rowCount = keyboard_symbol_mode ? 4 : kb_row_count;

    for (int r = 0; r < rowCount; r++) {
        int rowLen = strlen(rows[r]);
        int startX = (540 - rowLen * (keyW + spacing)) / 2;
        int ky = startY + r * (keyH + spacing);

        for (int c = 0; c < rowLen; c++) {
            int x = startX + c * (keyW + spacing);
            canvas.drawRect(x, ky, keyW, keyH, 15);
            
            char ch = rows[r][c];
            if (!keyboard_symbol_mode && keyboard_caps && ch >= 'a' && ch <= 'z') {
                ch = ch - 'a' + 'A';
            }
            char str[2] = {ch, 0};
            if (ch == ' ') {
                canvas.drawString("SP", x + 6, ky + 18);
            } else {
                canvas.drawString(str, x + 12, ky + 18);
            }
        }
    }

    // 機能ボタン
    int btnY = startY + rowCount * (keyH + spacing) + 12;
    int btnH = 52;
    canvas.setTextSize(22);
    
    // 7ボタン: BS CAPS !@# DEL CLR OK ESC
    // 幅74px, 間隔4px → 7*74+6*4 = 542
    int bw = 74;
    int bx = 3;
    int bgap = 4;
    
    // BS (バックスペース)
    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("BS", bx + 22, btnY + 14);
    bx += bw + bgap;
    
    // CAPS
    canvas.drawRect(bx, btnY, bw, btnH, 15);
    if (keyboard_caps) canvas.fillRect(bx+2, btnY+2, bw-4, btnH-4, 8);
    canvas.drawString("CAP", bx + 14, btnY + 14);
    bx += bw + bgap;
    
    // 123/ABC
    canvas.drawRect(bx, btnY, bw, btnH, 15);
    if (keyboard_symbol_mode) canvas.fillRect(bx+2, btnY+2, bw-4, btnH-4, 8);
    canvas.drawString(keyboard_symbol_mode ? "ABC" : "!@#", bx + 14, btnY + 14);
    bx += bw + bgap;
    
    // DEL (全消去)
    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("CLR", bx + 14, btnY + 14);
    bx += bw + bgap;
    
    // SP (スペース)
    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("SP", bx + 22, btnY + 14);
    bx += bw + bgap;
    
    // OK
    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("OK", bx + 22, btnY + 14);
    bx += bw + bgap;
    
    // ESC
    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("ESC", bx + 16, btnY + 14);

    // フッター
    canvas.setTextSize(20);
    drawText("タップで入力 / OK:保存 / ESC:キャンセル", 10, 925);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);  // GC16で残像防止
}

int getKeyboardHit(int tx, int ty) {
    int startY = 470;
    int keyW = 42;
    int keyH = 60;
    int spacing = 3;

    const char** rows = keyboard_symbol_mode ? kb_symbol_rows : kb_rows;
    int rowCount = keyboard_symbol_mode ? 4 : kb_row_count;

    // キー判定
    for (int r = 0; r < rowCount; r++) {
        int rowLen = strlen(rows[r]);
        int startX = (540 - rowLen * (keyW + spacing)) / 2;
        int y = startY + r * (keyH + spacing);

        if (ty >= y && ty < y + keyH) {
            for (int c = 0; c < rowLen; c++) {
                int x = startX + c * (keyW + spacing);
                if (tx >= x && tx < x + keyW) {
                    return (r << 8) | c;
                }
            }
        }
    }

    // 機能ボタン判定 (7ボタン: BS CAPS !@# CLR SP OK ESC)
    int btnY = startY + rowCount * (keyH + spacing) + 12;
    int btnH = 52;
    int bw = 74;
    int bgap = 4;
    if (ty >= btnY && ty < btnY + btnH) {
        int bx = 3;
        if (tx >= bx && tx < bx + bw) return -2;      // BS (backspace)
        bx += bw + bgap;
        if (tx >= bx && tx < bx + bw) return -1;      // CAPS
        bx += bw + bgap;
        if (tx >= bx && tx < bx + bw) return -6;      // 123/ABC
        bx += bw + bgap;
        if (tx >= bx && tx < bx + bw) return -3;      // CLR
        bx += bw + bgap;
        if (tx >= bx && tx < bx + bw) return -7;      // SP (space)
        bx += bw + bgap;
        if (tx >= bx && tx < bx + bw) return -4;      // OK
        bx += bw + bgap;
        if (tx >= bx && tx < bx + bw) return -5;      // ESC
    }

    return -100;  // no hit
}

void processKeyboardHit(int hit) {
    if (hit == -100) return;

    if (hit == -1) {
        // CAPS
        keyboard_caps = !keyboard_caps;
    } else if (hit == -6) {
        // 123/ABC toggle
        keyboard_symbol_mode = !keyboard_symbol_mode;
    } else if (hit == -2) {
        // DEL
        if (keyboard_buffer.length() > 0) {
            keyboard_buffer.remove(keyboard_buffer.length() - 1);
        }
    } else if (hit == -3) {
        // CLR
        keyboard_buffer = "";
    } else if (hit == -4) {
        // OK - 保存して戻る
        switch (keyboard_target) {
            case SET_WIFI_SSID:
                strlcpy(config.wifi_ssid, keyboard_buffer.c_str(), sizeof(config.wifi_ssid));
                break;
            case SET_WIFI_PASS:
                strlcpy(config.wifi_pass, keyboard_buffer.c_str(), sizeof(config.wifi_pass));
                break;
            case SET_ICS_URL:
                strlcpy(config.ics_url, keyboard_buffer.c_str(), sizeof(config.ics_url));
                break;
            case SET_ICS_USER:
                strlcpy(config.ics_user, keyboard_buffer.c_str(), sizeof(config.ics_user));
                break;
            case SET_ICS_PASS:
                strlcpy(config.ics_pass, keyboard_buffer.c_str(), sizeof(config.ics_pass));
                break;
            case SET_MIDI_URL:
                strlcpy(config.midi_url, keyboard_buffer.c_str(), sizeof(config.midi_url));
                break;
        }
        keyboard_symbol_mode = false;
        keyboard_caps = false;
        ui_state = UI_SETTINGS;
        drawSettings();
        return;
    } else if (hit == -5) {
        // ESC - キャンセル
        keyboard_symbol_mode = false;
        keyboard_caps = false;
        ui_state = UI_SETTINGS;
        drawSettings();
        return;
    } else if (hit == -7) {
        // SP - スペース追加
        keyboard_buffer += ' ';
    } else {
        // 通常キー
        const char** rows = keyboard_symbol_mode ? kb_symbol_rows : kb_rows;
        int rowCount = keyboard_symbol_mode ? 4 : kb_row_count;
        int r = (hit >> 8) & 0xFF;
        int c = hit & 0xFF;
        if (r < rowCount && c < (int)strlen(rows[r])) {
            char ch = rows[r][c];
            if (!keyboard_symbol_mode && keyboard_caps && ch >= 'a' && ch <= 'z') {
                ch = ch - 'a' + 'A';
            }
            keyboard_buffer += ch;
        }
    }
    drawKeyboard();
}

//==============================================================================
// MIDIファイル選択画面
//==============================================================================
void drawMidiSelect() {
    canvas.fillCanvas(0);  // 白
    canvas.setTextColor(15);  // 黒
    canvas.setTextDatum(TL_DATUM);
    
    canvas.setTextSize(26);
    drawText("=== MIDIファイル選択 ===", 10, 10);

    canvas.setTextSize(22);
    int y = 55;
    int rowH = 38;

    for (int i = 0; i < midi_file_count && y < 880; i++) {
        if (i == midi_select_cursor) {
            canvas.fillRect(0, y - 2, 540, rowH - 2, 4);
        }
        drawText(midi_files[i], 10, y);
        y += rowH;
    }

    if (midi_file_count == 0) {
        drawText("/midi/ にMIDIファイルなし", 10, 100);
    }

    canvas.setTextSize(20);
    drawText("L:上 R:下 P:選択 タップ:戻る", 10, 920);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

//==============================================================================
// ボーレート選択画面
//==============================================================================
void drawBaudSelect() {
    canvas.fillCanvas(0);  // 白
    canvas.setTextColor(15);  // 黒
    canvas.setTextDatum(TL_DATUM);
    
    canvas.setTextSize(26);
    drawText("=== MIDIボーレート選択 ===", 10, 10);

    canvas.setTextSize(30);
    int y = 80;
    int rowH = 55;

    for (int i = 0; i < baud_option_count; i++) {
        if (i == baud_select_cursor) {
            canvas.fillRect(0, y - 5, 540, rowH - 5, 4);
        }
        drawText(String(baud_options[i]), 20, y);
        y += rowH;
    }

    canvas.setTextSize(20);
    drawText("L:上 R:下 P:選択", 10, 920);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

//==============================================================================
// ポート選択画面
//==============================================================================
void drawPortSelect() {
    canvas.fillCanvas(0);  // 白
    canvas.setTextColor(15);  // 黒
    canvas.setTextDatum(TL_DATUM);
    
    canvas.setTextSize(26);
    drawText("=== ポート選択 ===", 10, 10);

    canvas.setTextSize(26);
    int y = 80;
    int rowH = 55;

    for (int i = 0; i < 3; i++) {
        if (i == port_select_cursor) {
            canvas.fillRect(0, y - 5, 540, rowH - 5, 4);
        }
        drawText(port_names[i], 20, y);
        y += rowH;
    }

    canvas.setTextSize(20);
    drawText("L:上 R:下 P:選択", 10, 920);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

//==============================================================================
// スイッチ処理
//==============================================================================
void checkSwitches() {
    bool sw_l = digitalRead(SW_L_PIN);
    bool sw_r = digitalRead(SW_R_PIN);
    bool sw_p = digitalRead(SW_P_PIN);

    // Lスイッチ（上/戻る）
    if (!sw_l && sw_l_prev) {
        Serial.println("SW_L pressed");
        last_interaction_ms = millis();
        handleSwitch('L');
    }

    // Rスイッチ（下/進む）
    if (!sw_r && sw_r_prev) {
        Serial.println("SW_R pressed");
        last_interaction_ms = millis();
        handleSwitch('R');
    }

    // Pスイッチ（決定/メニュー）
    if (!sw_p && sw_p_prev) {
        Serial.println("SW_P pressed");
        last_interaction_ms = millis();
        handleSwitch('P');
    }

    sw_l_prev = sw_l;
    sw_r_prev = sw_r;
    sw_p_prev = sw_p;
}

void handleSwitch(char sw) {
    switch (ui_state) {
        case UI_LIST:
            if (sw == 'P') {
                ui_state = UI_SETTINGS;
                settings_cursor = 0;
                drawSettings();
            } else if (sw == 'L') {
                if (event_count > 0) {
                    if (selected_event > 0) {
                        selected_event--;
                        // ページを戻す
                        if (selected_event < page_start) {
                            page_start = max(0, page_start - ITEMS_PER_PAGE);
                        }
                    } else {
                        // 最後に移動
                        selected_event = event_count - 1;
                        page_start = max(0, event_count - ITEMS_PER_PAGE);
                    }
                    drawList();
                }
            } else if (sw == 'R') {
                if (event_count > 0) {
                    if (selected_event < event_count - 1) {
                        selected_event++;
                        // ページを進める
                        if (selected_event >= page_start + ITEMS_PER_PAGE) {
                            page_start += ITEMS_PER_PAGE;
                        }
                    } else {
                        // 最初に移動
                        selected_event = 0;
                        page_start = 0;
                    }
                    drawList();
                }
            }
            break;

        case UI_DETAIL:
            if (sw == 'P') {
                ui_state = UI_LIST;
                drawList();
            } else if (sw == 'L') {
                if (detail_scroll > 0) {
                    detail_scroll--;
                    drawDetail(selected_event);
                }
            } else if (sw == 'R') {
                detail_scroll++;
                drawDetail(selected_event);
            }
            break;

        case UI_PLAYING:
            // 再生中はタップのみで停止
            break;

        case UI_SETTINGS:
            if (sw == 'L') {
                if (settings_cursor > 0) {
                    settings_cursor--;
                    drawSettings();
                } else {
                    // 一番上でLを押すと戻る
                    ui_state = UI_LIST;
                    scrollToToday();
                    drawList();
                }
            } else if (sw == 'R') {
                settings_cursor = (settings_cursor + 1) % SET_COUNT;
                drawSettings();
            } else if (sw == 'P') {
                handleSettingsSelect();
            }
            break;

        case UI_MIDI_SELECT:
            if (sw == 'L') {
                if (midi_file_count > 0) {
                    midi_select_cursor = (midi_select_cursor <= 0) ? midi_file_count - 1 : midi_select_cursor - 1;
                    drawMidiSelect();
                }
            } else if (sw == 'R') {
                if (midi_file_count > 0) {
                    midi_select_cursor = (midi_select_cursor + 1) % midi_file_count;
                    drawMidiSelect();
                }
            } else if (sw == 'P') {
                if (midi_file_count > 0) {
                    strlcpy(config.midi_file, midi_files[midi_select_cursor].c_str(), sizeof(config.midi_file));
                }
                ui_state = UI_SETTINGS;
                drawSettings();
            }
            break;

        case UI_BAUD_SELECT:
            if (sw == 'L') {
                baud_select_cursor = (baud_select_cursor <= 0) ? baud_option_count - 1 : baud_select_cursor - 1;
                drawBaudSelect();
            } else if (sw == 'R') {
                baud_select_cursor = (baud_select_cursor + 1) % baud_option_count;
                drawBaudSelect();
            } else if (sw == 'P') {
                config.midi_baud = baud_options[baud_select_cursor];
                Serial2.updateBaudRate(config.midi_baud);
                ui_state = UI_SETTINGS;
                drawSettings();
            }
            break;

        case UI_PORT_SELECT:
            if (sw == 'L') {
                port_select_cursor = (port_select_cursor <= 0) ? 2 : port_select_cursor - 1;
                drawPortSelect();
            } else if (sw == 'R') {
                port_select_cursor = (port_select_cursor + 1) % 3;
                drawPortSelect();
            } else if (sw == 'P') {
                config.port_select = port_select_cursor;
                // ポート変更時はSerial2を再初期化
                Serial2.end();
                Serial2.begin(config.midi_baud, SERIAL_8N1, -1, port_tx_pins[config.port_select]);
                ui_state = UI_SETTINGS;
                drawSettings();
            }
            break;

        case UI_KEYBOARD:
            // キーボードはタッチのみ
            break;
    }
}

void handleSettingsSelect() {
    switch (settings_cursor) {
        case SET_WIFI_SSID:
            keyboard_target = SET_WIFI_SSID;
            keyboard_buffer = config.wifi_ssid;
            ui_state = UI_KEYBOARD;
            drawKeyboard();
            break;

        case SET_WIFI_PASS:
            keyboard_target = SET_WIFI_PASS;
            keyboard_buffer = config.wifi_pass;
            ui_state = UI_KEYBOARD;
            drawKeyboard();
            break;

        case SET_ICS_URL:
            keyboard_target = SET_ICS_URL;
            keyboard_buffer = config.ics_url;
            ui_state = UI_KEYBOARD;
            drawKeyboard();
            break;

        case SET_ICS_USER:
            keyboard_target = SET_ICS_USER;
            keyboard_buffer = config.ics_user;
            ui_state = UI_KEYBOARD;
            drawKeyboard();
            break;

        case SET_ICS_PASS:
            keyboard_target = SET_ICS_PASS;
            keyboard_buffer = config.ics_pass;
            ui_state = UI_KEYBOARD;
            drawKeyboard();
            break;

        case SET_MIDI_FILE:
            scanMidiFiles();
            midi_select_cursor = 0;
            // 現在のファイルを選択状態に
            for (int i = 0; i < midi_file_count; i++) {
                if (midi_files[i] == config.midi_file) {
                    midi_select_cursor = i;
                    break;
                }
            }
            ui_state = UI_MIDI_SELECT;
            drawMidiSelect();
            break;

        case SET_MIDI_URL:
            keyboard_target = SET_MIDI_URL;
            keyboard_buffer = config.midi_url;
            ui_state = UI_KEYBOARD;
            drawKeyboard();
            break;

        case SET_MIDI_BAUD:
            baud_select_cursor = 0;
            for (int i = 0; i < baud_option_count; i++) {
                if (baud_options[i] == config.midi_baud) {
                    baud_select_cursor = i;
                    break;
                }
            }
            ui_state = UI_BAUD_SELECT;
            drawBaudSelect();
            break;

        case SET_PORT:
            port_select_cursor = config.port_select;
            ui_state = UI_PORT_SELECT;
            drawPortSelect();
            break;

        case SET_ALARM_OFFSET:
            // 簡易的に5分単位で変更
            config.alarm_offset_default = (config.alarm_offset_default + 5) % 65;
            drawSettings();
            break;

        case SET_TIME_FORMAT:
            // 24時間制/12時間制切り替え
            config.time_24h = !config.time_24h;
            drawSettings();
            break;

        case SET_TEXT_WRAP:
            // 折り返し/切り詰め切り替え
            config.text_wrap = !config.text_wrap;
            drawSettings();
            break;

        case SET_ICS_POLL: {
            // 更新頻度: 5, 10, 15, 30, 60分
            const int poll_opts[] = {5, 10, 15, 30, 60};
            const int poll_count = 5;
            int cur = 0;
            for (int j = 0; j < poll_count; j++) {
                if (poll_opts[j] == config.ics_poll_min) { cur = j; break; }
            }
            cur = (cur + 1) % poll_count;
            config.ics_poll_min = poll_opts[cur];
            drawSettings();
            break;
        }

        case SET_PLAY_DURATION: {
            // 鳴動時間: 1曲(0), 5, 10, 15, 20秒
            const int dur_opts[] = {0, 5, 10, 15, 20};
            const int dur_cnt = 5;
            int cur = 0;
            for (int j = 0; j < dur_cnt; j++) {
                if (dur_opts[j] == config.play_duration) { cur = j; break; }
            }
            cur = (cur + 1) % dur_cnt;
            config.play_duration = dur_opts[cur];
            drawSettings();
            break;
        }

        case SET_PLAY_REPEAT: {
            // 繰り返し: 1, 2, 3, 4, 5回
            config.play_repeat = (config.play_repeat % 5) + 1;
            drawSettings();
            break;
        }

        case SET_ICS_UPDATE:
            // ICS再取得
            canvas.fillCanvas(0);
            canvas.setTextColor(15);
            canvas.setTextDatum(MC_DATUM);
            canvas.setTextSize(28);
            canvas.drawString("ICS取得中...", 270, 280);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            
            if (WiFi.status() != WL_CONNECTED) {
                connectWiFi();
            }
            fetchAndUpdate();
            drawSettings();
            break;

        case SET_SOUND_TEST: {
            // サウンドテスト: 本番アラームと同じ非同期再生経路でテスト
            // startMidiPlayback → loop()内updateMidiPlayback → finishAlarm
            Serial.println("\n*** SOUND TEST ***");
            Serial.printf("  MIDI: %s\n", config.midi_file);
            Serial.printf("  Exists: %s\n", SD.exists(config.midi_file) ? "YES" : "NO");
            Serial.printf("  Heap: %d\n", ESP.getFreeHeap());
            
            if (!SD.exists(config.midi_file)) {
                Serial.println("  => MIDI file not found!");
                canvas.fillCanvas(0);
                canvas.setTextColor(15);
                canvas.setTextDatum(MC_DATUM);
                canvas.setTextSize(28);
                canvas.drawString("MIDI再生失敗", 270, 400);
                canvas.setTextSize(22);
                canvas.drawString(config.midi_file, 270, 450);
                canvas.drawString("ファイルを確認してください", 270, 490);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
                delay(3000);
                drawSettings();
                break;
            }
            
            // 鳴動パラメータ設定（本番と同じ）
            int dur = config.play_duration;
            play_duration_ms = dur * 1000;
            int rep = config.play_repeat;
            if (rep < 1) rep = 1;
            play_repeat_remaining = rep;
            play_start_ms = millis();
            playing_event = -1;  // テスト用（特定イベントなし）
            
            Serial.printf("  Duration: %s, Repeat: %d\n",
                          dur == 0 ? "1song" : (String(dur) + "sec").c_str(), rep);
            
            if (startMidiPlayback(config.midi_file)) {
                Serial.println("  => Playback started OK");
                ui_state = UI_PLAYING;
                // テスト用の再生画面
                canvas.fillCanvas(0);
                canvas.setTextColor(15);
                canvas.setTextDatum(MC_DATUM);
                canvas.setTextSize(48);
                canvas.drawString("SOUND TEST", 270, 200);
                canvas.setTextSize(24);
                canvas.drawString(config.midi_file, 270, 300);
                String info = "";
                if (dur > 0) info += String(dur) + "秒";
                else info += "1曲";
                info += " x" + String(rep) + "回";
                canvas.drawString(info, 270, 350);
                canvas.setTextSize(28);
                canvas.drawString("タップで停止", 270, 450);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            } else {
                Serial.println("  => Playback FAILED!");
                canvas.fillCanvas(0);
                canvas.setTextColor(15);
                canvas.setTextDatum(MC_DATUM);
                canvas.setTextSize(28);
                canvas.drawString("MIDI再生失敗", 270, 400);
                canvas.setTextSize(22);
                canvas.drawString(config.midi_file, 270, 450);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
                delay(3000);
                drawSettings();
            }
            break;
        }

        case SET_SAVE_EXIT:
            saveConfig();
            ui_state = UI_LIST;
            scrollToToday();
            drawList();
            break;
    }
}

//==============================================================================
// タッチ処理
//==============================================================================
void handleTouch(int tx, int ty) {
    last_interaction_ms = millis();
    switch (ui_state) {
        case UI_LIST: {
            // ボタンチェック
            if (ty >= btn_prev.y0 && ty <= btn_prev.y1) {
                if (tx >= btn_prev.x0 && tx <= btn_prev.x1) {
                    // 前日：表示先頭を前の日付に変更
                    if (page_start > 0) {
                        struct tm cur_tm;
                        localtime_r(&events[page_start].start, &cur_tm);
                        int cur_day = cur_tm.tm_mday + cur_tm.tm_mon * 100 + cur_tm.tm_year * 10000;
                        
                        // 前の日付の先頭を探す
                        int found = -1;
                        for (int i = page_start - 1; i >= 0; i--) {
                            struct tm t;
                            localtime_r(&events[i].start, &t);
                            int day = t.tm_mday + t.tm_mon * 100 + t.tm_year * 10000;
                            if (day < cur_day) {
                                // この日付の先頭を探す
                                found = i;
                                for (int j = i - 1; j >= 0; j--) {
                                    struct tm t2;
                                    localtime_r(&events[j].start, &t2);
                                    int day2 = t2.tm_mday + t2.tm_mon * 100 + t2.tm_year * 10000;
                                    if (day2 == day) found = j;
                                    else break;
                                }
                                break;
                            }
                        }
                        if (found >= 0) {
                            page_start = found;
                            selected_event = found;
                        } else {
                            page_start = 0;
                            selected_event = 0;
                        }
                        drawList();
                    }
                    return;
                } else if (tx >= btn_next.x0 && tx <= btn_next.x1) {
                    // 翌日：表示先頭を次の日付に変更
                    if (page_start < event_count - 1) {
                        struct tm cur_tm;
                        localtime_r(&events[page_start].start, &cur_tm);
                        int cur_day = cur_tm.tm_mday + cur_tm.tm_mon * 100 + cur_tm.tm_year * 10000;
                        
                        // 次の日付の先頭を探す
                        for (int i = page_start + 1; i < event_count; i++) {
                            struct tm t;
                            localtime_r(&events[i].start, &t);
                            int day = t.tm_mday + t.tm_mon * 100 + t.tm_year * 10000;
                            if (day > cur_day) {
                                page_start = i;
                                selected_event = i;
                                break;
                            }
                        }
                        drawList();
                    }
                    return;
                } else if (tx >= btn_today.x0 && tx <= btn_today.x1) {
                    // 今日：表示先頭を今日に変更
                    time_t now = time(nullptr);
                    struct tm now_tm;
                    localtime_r(&now, &now_tm);
                    int today = now_tm.tm_mday + now_tm.tm_mon * 100 + now_tm.tm_year * 10000;
                    
                    for (int i = 0; i < event_count; i++) {
                        struct tm t;
                        localtime_r(&events[i].start, &t);
                        int day = t.tm_mday + t.tm_mon * 100 + t.tm_year * 10000;
                        if (day >= today) {
                            page_start = i;
                            selected_event = i;
                            break;
                        }
                    }
                    drawList();
                    return;
                } else if (tx >= btn_detail.x0 && tx <= btn_detail.x1) {
                    // 詳細
                    if (selected_event >= 0 && selected_event < event_count) {
                        ui_state = UI_DETAIL;
                        detail_scroll = 0;
                        drawDetail(selected_event);
                    }
                    return;
                }
            }
            
            // 日付ヘッダーをタップした場合は無視
            for (int i = 0; i < date_header_count; i++) {
                if (ty >= date_header_y0[i] && ty <= date_header_y1[i]) {
                    return;
                }
            }
            
            // 予定行をタップ → 詳細表示
            for (int i = 0; i < displayed_count; i++) {
                if (ty >= row_y0[i] && ty <= row_y1[i]) {
                    selected_event = row_event_idx[i];
                    ui_state = UI_DETAIL;
                    detail_scroll = 0;
                    drawDetail(selected_event);
                    return;
                }
            }
            break;
        }

        case UI_DETAIL:
            ui_state = UI_LIST;
            drawList();
            break;

        case UI_PLAYING:
            finishAlarm();
            break;

        case UI_KEYBOARD: {
            int hit = getKeyboardHit(tx, ty);
            processKeyboardHit(hit);
            break;
        }

        case UI_MIDI_SELECT:
            ui_state = UI_SETTINGS;
            drawSettings();
            break;

        case UI_BAUD_SELECT:
            ui_state = UI_SETTINGS;
            drawSettings();
            break;

        case UI_PORT_SELECT:
            ui_state = UI_SETTINGS;
            drawSettings();
            break;

        case UI_SETTINGS:
            // ナビゲーションボタン判定
            if (ty >= 900 && ty < 948) {
                if (tx >= 5 && tx < 135) {
                    // 先頭
                    settings_cursor = 0;
                    drawSettings();
                    return;
                }
                if (tx >= 145 && tx < 275) {
                    // 末尾
                    settings_cursor = SET_COUNT - 1;
                    drawSettings();
                    return;
                }
                if (tx >= 285 && tx < 415) {
                    // 戻る（保存せず一覧へ）
                    ui_state = UI_LIST;
                    scrollToToday();
                    drawList();
                    return;
                }
            }
            // タッチで項目選択（先頭=カーソル位置からのオフセット）
            {
                int itemY = 45;
                int rowH = 50;
                int maxVisible = (895 - itemY) / rowH;
                for (int n = 0; n < maxVisible; n++) {
                    int i = settings_cursor + n;
                    if (i >= SET_COUNT) break;
                    if (ty >= itemY && ty < itemY + rowH) {
                        settings_cursor = i;
                        handleSettingsSelect();
                        return;
                    }
                    itemY += rowH;
                }
            }
            break;
    }
}

//==============================================================================
// アラームチェック
// triggeredフラグの説明:
//   ICS更新は30分間隔のため、発火済みアラームもリストに残る。
//   triggeredフラグで同じアラームが繰り返し発火するのを防止する。
//   ICS再取得時に全イベントが再構築され、過去のアラームは
//   parseICS内で triggered=true に初期化される。
//==============================================================================
void checkAlarms() {
    if (midi_playing) return;  // 再生中はスキップ

    time_t now = time(nullptr);
    
    // 毎分アラームリストをシリアル出力（デバッグ：未発火の未来アラームのみ）
    if (now - last_alarm_debug >= 60) {
        last_alarm_debug = now;
        struct tm lt;
        localtime_r(&now, &lt);
        Serial.printf("\n=== ALARM CHECK [%02d/%02d %02d:%02d:%02d] ===\n",
                      lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
        
        int pending_count = 0;
        for (int i = 0; i < event_count; i++) {
            if (!events[i].has_alarm) continue;
            if (events[i].triggered) continue;  // 発火済みはスキップ
            
            pending_count++;
            struct tm at;
            localtime_r(&events[i].alarm_time, &at);
            struct tm st;
            localtime_r(&events[i].start, &st);
            
            long remain_sec = (long)(events[i].alarm_time - now);
            
            Serial.printf("  [%d] %s\n", i, events[i].summary.c_str());
            Serial.printf("      event:%02d/%02d %02d:%02d  alarm:%02d/%02d %02d:%02d  "
                          "offset:%dmin  remain:%ldsec\n",
                          st.tm_mon+1, st.tm_mday, st.tm_hour, st.tm_min,
                          at.tm_mon+1, at.tm_mday, at.tm_hour, at.tm_min,
                          events[i].offset_min, remain_sec);
            if (events[i].midi_file.length() > 0) {
                Serial.printf("      midi:%s (%s)\n", 
                              events[i].midi_file.c_str(),
                              events[i].midi_is_url ? "URL" : "SD");
            }
        }
        if (pending_count == 0) {
            Serial.println("  (no pending alarms)");
        }
        Serial.printf("=== events:%d, pending alarms:%d, heap:%d ===\n\n",
                      event_count, pending_count, ESP.getFreeHeap());
    }
    
    for (int i = 0; i < event_count; i++) {
        // %AL%マーカーがある予定のみアラームをチェック
        if (events[i].has_alarm && !events[i].triggered && events[i].alarm_time <= now) {
            // アラーム発火！
            Serial.printf("\n*** ALARM FIRING! ***\n");
            Serial.printf("  Event: %s\n", events[i].summary.c_str());
            Serial.printf("  alarm_time <= now : %ld <= %ld\n", 
                          (long)events[i].alarm_time, (long)now);
            
            playing_event = i;
            
            // 鳴動時間を決定（イベント指定 > 設定値）
            int dur = events[i].play_duration_sec;
            if (dur < 0) dur = config.play_duration;  // 設定値使用
            play_duration_ms = dur * 1000;  // 0=1曲（時間制限なし）
            
            // 繰り返し回数を決定
            int rep = events[i].play_repeat;
            if (rep < 0) rep = config.play_repeat;  // 設定値使用
            if (rep < 1) rep = 1;
            play_repeat_remaining = rep;
            
            Serial.printf("  Duration: %s, Repeat: %d\n", 
                          dur == 0 ? "1song" : (String(dur) + "sec").c_str(),
                          play_repeat_remaining);
            
            // イベント固有のMIDIファイルを取得
            String midiPath = getMidiPath(i);
            Serial.printf("  MIDI file: %s\n", midiPath.c_str());
            Serial.printf("  File exists: %s\n", SD.exists(midiPath.c_str()) ? "YES" : "NO");
            
            play_start_ms = millis();
            if (startMidiPlayback(midiPath.c_str())) {
                Serial.println("  => Playback started OK");
                ui_state = UI_PLAYING;
                drawPlaying(i);
            } else {
                Serial.println("  => Playback FAILED!");
                // MIDI再生失敗時もトリガー済みにする
                events[i].triggered = true;
            }
            break;
        }
    }
}

//==============================================================================
// セットアップ
//==============================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== M5Paper Alarm starting... ===");

    M5.begin();
    M5.TP.SetRotation(90);
    M5.EPD.SetRotation(90);
    M5.EPD.Clear(true);

    // スイッチピン設定
    pinMode(SW_L_PIN, INPUT_PULLUP);
    pinMode(SW_R_PIN, INPUT_PULLUP);
    pinMode(SW_P_PIN, INPUT_PULLUP);

    // SD初期化
    if (!SD.begin(4)) {
        Serial.println("SD init failed!");
        canvas.createCanvas(540, 960);
        canvas.setTextSize(32);
        canvas.drawString("SD Card Error!", 10, 100);
        canvas.pushCanvas(0, 0, UPDATE_MODE_DU4);
        while (1) delay(1000);
    }
    Serial.println("SD initialized");

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
    canvas.fillCanvas(0);  // 白
    canvas.setTextColor(15);  // 黒
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(32);
    canvas.drawString("M5Paper Alarm", 270, 180);
    canvas.setTextSize(24);
    canvas.drawString("Connecting WiFi...", 270, 240);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    // WiFi接続
    if (connectWiFi()) {
        // 時刻同期
        setenv("TZ", TZ_JST, 1);
        tzset();
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        
        canvas.drawString("WiFi OK! Syncing time...", 270, 280);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        
        // NTP同期待ち（最大10秒）
        time_t now = 0;
        int retry = 0;
        while (now < 1700000000 && retry < 20) {  // 2023年以降の値になるまで待つ
            delay(500);
            now = time(nullptr);
            retry++;
        }
        Serial.printf("NTP sync: %ld (retry: %d)\n", now, retry);

        // ICS取得
        canvas.drawString("Fetching calendar...", 270, 320);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        fetchAndUpdate();
        
        // last_fetchを現在時刻で確実に更新
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
            // 指が離れた瞬間のみ処理
            tp_finger_t p = M5.TP.readFinger(0);
            if (p.x > 0 && p.y > 0) {
                handleTouch(p.x, p.y);
            }
        }
        was_touched = is_touched;
        M5.TP.flush();
    }

    // アラームチェック（再生中でなければ）
    if (ui_state != UI_PLAYING && ui_state != UI_SETTINGS && 
        ui_state != UI_KEYBOARD) {
        checkAlarms();
    }

    // 操作なし3分以上 かつ UI_LIST の場合、毎分自動で今日の先頭へスクロール＋再描画
    {
        time_t now_t = time(nullptr);
        bool idle = (millis() - last_interaction_ms) > 180000;  // 3分
        
        if (idle && ui_state == UI_LIST && now_t != (time_t)-1 &&
            (now_t - last_auto_refresh) >= 60) {
            last_auto_refresh = now_t;
            scrollToToday();
            Serial.printf("AUTO-REFRESH: idle=%lus, scroll to today (idx=%d)\n",
                          (millis() - last_interaction_ms) / 1000, page_start);
            drawList();
        }
    }

    // 定期ICS更新（失敗時はバックオフ）
    time_t now = time(nullptr);
    // 連続失敗時は間隔を延長（最大30分）
    int poll_interval = (config.ics_poll_min * 60) * (1 + min(fetch_fail_count, 5));
    if (now != (time_t)-1 && (now - last_fetch) >= poll_interval) {
        if (WiFi.status() != WL_CONNECTED) {
            if (!connectWiFi()) {
                // WiFi接続失敗時も last_fetch を更新
                last_fetch = now;
                fetch_fail_count++;
            }
        }
        if (WiFi.status() == WL_CONNECTED) {
            fetchAndUpdate();
            
            if (ui_state == UI_LIST) {
                scrollToToday();
                drawList();
            }
        }
    }

    delay(1);
}
