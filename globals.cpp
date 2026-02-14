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
bool reboot_pending = false;
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
