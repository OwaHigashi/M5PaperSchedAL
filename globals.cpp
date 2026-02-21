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
int play_repeat_remaining = 0;
unsigned long play_start_ms = 0;
int play_duration_ms = 0;

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
int fetch_fail_count = 0;
bool debug_fetch = false;
bool reboot_pending = false;
DisplayRow last_pushed[MAX_DISPLAY_ROWS];
int last_pushed_count = 0;
bool row_changed[MAX_DISPLAY_ROWS];
unsigned long last_switch_check = 0;
unsigned long last_interaction_ms = 0;
time_t last_alarm_debug = 0;
time_t last_auto_refresh = 0;
unsigned long last_sd_check_ms = 0;

SimpleMIDIPlayer midi;
bool midi_playing = false;

bool sd_healthy = true;

bool sw_l_prev = true;
bool sw_r_prev = true;
bool sw_p_prev = true;

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
        mark = events[evtIdx].triggered ? "*" : "♪";
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
