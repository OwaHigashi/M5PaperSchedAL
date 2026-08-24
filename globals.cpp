#include "globals.h"

//==============================================================================
// グローバル変数の実体定義
//==============================================================================

M5EPD_Canvas canvas(&M5.EPD);

Config config;
EventItem* events = nullptr;
int event_count = 0;

UiState ui_state = UI_LIST;
int selected_event = -1;
int page_start = 0;
int displayed_count = 0;
int row_event_idx[MAX_EVENTS];
int detail_scroll = 0;

ButtonArea btn_prev, btn_next, btn_today, btn_detail;
int settings_cursor = 0;

int playing_event = -1;
int playing_alarm_idx = -1;
char playing_alarm_id[40] = "";
int play_repeat_remaining = 0;
unsigned long play_start_ms = 0;
int play_duration_ms = 0;
char play_file_override[96] = "";

String keyboard_buffer;
int keyboard_target = -1;
int keyboard_cursor = 0;

String midi_files[32];
int midi_file_count = 0;
int midi_select_cursor = 0;

extern const uint32_t baud_options[] = {31250, 31520, 38400};
int baud_select_cursor = 0;

extern const char* port_names[] = {"PORT A (G25)", "PORT B (G26)", "PORT C (G18)"};
extern const int port_tx_pins[] = {25, 26, 18};
int port_select_cursor = 0;

int row_y0[MAX_EVENTS];
int row_y1[MAX_EVENTS];

int date_header_y0[10];
int date_header_y1[10];
int date_header_count = 0;

time_t last_fetch = 0;
int sync_fail_count = 0;
unsigned long host_lost_since_ms = 0;
bool host_online = false;
long host_rev = -1;
long local_rev = -1;
int src_fail_mask = 0;
unsigned long hb_interval_ms = HB_INTERVAL_MS_DEFAULT;
int full_sync_sec = FULL_SYNC_SEC_DEFAULT;
time_t host_next_alarm = 0;
bool reboot_pending = false;
DisplayRow last_pushed[MAX_DISPLAY_ROWS];
int last_pushed_count = 0;
bool row_changed[MAX_DISPLAY_ROWS];
unsigned long last_interaction_ms = 0;
time_t last_alarm_debug = 0;
time_t last_auto_refresh = 0;
time_t last_gc16_cleanup = 0;
bool time_valid = false;
bool events_cache_dirty = false;   // triggered変化後にキャッシュ再保存が必要

SimpleMIDIPlayer midi;
bool midi_playing = false;

bool fs_ok = false;

M5EPD_Canvas heartbeat_canvas(&M5.EPD);
bool heartbeat_visible = false;
unsigned long last_heartbeat_ms = 0;

int displayed_next_event_idx = -1;
int partial_refresh_count = 0;


//==============================================================================
// 表示内容スナップショット（画面イメージベース比較）
//   drawEventRow()と同じロジックで「画面に表示される文字列」を生成し保存
//   生データ(text[4000])ではなく、utf8Substringで切り詰めた表示文字列で比較
//   → len=153 vs len=120 でも画面上同一なら変更なしと判定
//==============================================================================
String computeRowDisplayText(int evtIdx) {
    if (evtIdx >= event_count) return "";

    struct tm st;
    localtime_r(&events[evtIdx].start, &st);

    // drawEventRow()と完全に同じフォーマット
    String timeStr = events[evtIdx].is_allday ? "[終日]" : formatTime(st.tm_hour, st.tm_min);
    String mark = "";
    if (events[evtIdx].has_alarm) {
        bool anyPending = false;
        for (int k = 0; k < events[evtIdx].alarm_count; k++) {
            if (!events[evtIdx].triggered[k]) { anyPending = true; break; }
        }
        mark = anyPending ? "♪" : "*";
    }

    String summary = removeUnsupportedChars(events[evtIdx].summary());
    int maxWidth = config.text_wrap ? 26 : 30;
    String dispSummary = utf8Substring(summary, maxWidth);

    String result = timeStr + "|" + mark + "|" + dispSummary;

    if (config.text_wrap && summary.length() > dispSummary.length()) {
        String rest = summary.substring(dispSummary.length());
        String line2 = utf8Substring(rest, 34);
        result += "|" + line2;
    }
    return result;
}

void saveDisplaySnapshot() {
    last_pushed_count = min(displayed_count, MAX_DISPLAY_ROWS);
    for (int d = 0; d < last_pushed_count; d++) {
        int idx = row_event_idx[d];
        String txt = computeRowDisplayText(idx);
        strncpy(last_pushed[d].display_text, txt.c_str(), DISPLAY_TEXT_LEN - 1);
        last_pushed[d].display_text[DISPLAY_TEXT_LEN - 1] = '\0';
    }
}

bool displayContentChanged() {
    memset(row_changed, 0, sizeof(row_changed));

    int count = min(displayed_count, MAX_DISPLAY_ROWS);
    if (count != last_pushed_count) {
        // 行数自体が変わった → 全行changed
        for (int d = 0; d < count; d++) row_changed[d] = true;
        return true;
    }

    bool any_changed = false;
    for (int d = 0; d < count; d++) {
        int idx = row_event_idx[d];
        String newText = computeRowDisplayText(idx);

        if (strcmp(last_pushed[d].display_text, newText.c_str()) != 0) {
            row_changed[d] = true;
            any_changed = true;
            Serial.printf("Row %d (event[%d]) display differs: '%s'\n",
                          d, idx, events[idx].summary());
        }
    }
    return any_changed;
}
