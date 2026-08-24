#include "globals.h"
#include <time.h>

//==============================================================================
// スイッチ処理
//   ポーリングだとハートビートHTTPやEPD描画のブロック中(数百ms)の押下を
//   取りこぼす → GPIO割り込みで押下をカウントし、ループ復帰後に必ず処理する
//==============================================================================
static volatile uint8_t sw_press_cnt[3] = {0, 0, 0};       // L, R, P
static volatile uint32_t sw_last_edge_ms[3] = {0, 0, 0};
static const uint32_t SW_DEBOUNCE_MS = 60;

static void IRAM_ATTR swISR(void* arg) {
    int idx = (int)(intptr_t)arg;
    uint32_t now = millis();
    if (now - sw_last_edge_ms[idx] < SW_DEBOUNCE_MS) return;   // チャタリング除去
    sw_last_edge_ms[idx] = now;
    if (sw_press_cnt[idx] < 8) sw_press_cnt[idx]++;            // 溜めすぎ防止
}

void initSwitchISR() {
    attachInterruptArg(SW_L_PIN, swISR, (void*)0, FALLING);
    attachInterruptArg(SW_R_PIN, swISR, (void*)1, FALLING);
    attachInterruptArg(SW_P_PIN, swISR, (void*)2, FALLING);
}

void checkSwitches() {
    static const char names[3] = {'L', 'R', 'P'};
    for (int i = 0; i < 3; i++) {
        while (sw_press_cnt[i] > 0) {
            noInterrupts(); sw_press_cnt[i]--; interrupts();
            char sw = names[i];
            Serial.printf("SW_%c pressed\n", sw);
            last_interaction_ms = millis();
            char info[2] = {sw, '\0'};
            uiEventPush("btn", 0, 0, info);
            handleSwitch(sw);
        }
    }
}

void handleSwitch(char sw) {
    switch (ui_state) {
        case UI_LIST:
            if (sw == 'P') {
                ui_state = UI_SETTINGS; settings_cursor = 0; drawSettings();
            } else if (sw == 'L') {
                if (event_count > 0) {
                    int old = selected_event;
                    if (selected_event > 0) {
                        selected_event--;
                        if (selected_event < page_start) {
                            page_start = max(0, selected_event - 2);
                            drawList();
                        } else {
                            updateListCursor(old, selected_event);
                        }
                    } else {
                        selected_event = event_count - 1;
                        page_start = max(0, event_count - displayed_count);
                        drawList();
                    }
                }
            } else if (sw == 'R') {
                if (event_count > 0) {
                    int old = selected_event;
                    if (selected_event < event_count - 1) {
                        selected_event++;
                        // 新カーソルが表示範囲外かチェック
                        bool on_screen = false;
                        for (int d = 0; d < displayed_count; d++) {
                            if (row_event_idx[d] == selected_event) { on_screen = true; break; }
                        }
                        if (!on_screen) {
                            page_start = max(0, selected_event - 2);
                            drawList();
                        } else {
                            updateListCursor(old, selected_event);
                        }
                    } else {
                        selected_event = 0; page_start = 0;
                        drawList();
                    }
                }
            }
            break;

        case UI_DETAIL:
            if (sw == 'P') { ui_state = UI_LIST; waitEPDReady(); drawList(false, false, false, true); }
            else if (sw == 'L') { if (detail_scroll > 0) { detail_scroll--; drawDetail(selected_event, true); } }
            else if (sw == 'R') { detail_scroll++; drawDetail(selected_event, true); }
            break;

        case UI_PLAYING:
            break;

        case UI_SETTINGS:
            if (sw == 'L') {
                if (settings_cursor > 0) { settings_cursor--; drawSettings(true); }
                else { ui_state = UI_LIST; scrollToToday(); drawList(); }
            } else if (sw == 'R') {
                settings_cursor = (settings_cursor + 1) % SET_COUNT; drawSettings(true);
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
                if (midi_file_count > 0) { midi_select_cursor = (midi_select_cursor + 1) % midi_file_count; drawMidiSelect(); }
            } else if (sw == 'P') {
                if (midi_file_count > 0) strlcpy(config.midi_file, midi_files[midi_select_cursor].c_str(), sizeof(config.midi_file));
                ui_state = UI_SETTINGS; drawSettings();
            }
            break;

        case UI_BAUD_SELECT:
            if (sw == 'L') { baud_select_cursor = (baud_select_cursor <= 0) ? BAUD_OPTION_COUNT - 1 : baud_select_cursor - 1; drawBaudSelect(); }
            else if (sw == 'R') { baud_select_cursor = (baud_select_cursor + 1) % BAUD_OPTION_COUNT; drawBaudSelect(); }
            else if (sw == 'P') {
                config.midi_baud = baud_options[baud_select_cursor];
                Serial2.updateBaudRate(config.midi_baud);
                ui_state = UI_SETTINGS; drawSettings();
            }
            break;

        case UI_PORT_SELECT:
            if (sw == 'L') { port_select_cursor = (port_select_cursor <= 0) ? 2 : port_select_cursor - 1; drawPortSelect(); }
            else if (sw == 'R') { port_select_cursor = (port_select_cursor + 1) % PORT_COUNT; drawPortSelect(); }
            else if (sw == 'P') {
                config.port_select = port_select_cursor;
                Serial2.end();
                Serial2.begin(config.midi_baud, SERIAL_8N1, -1, port_tx_pins[config.port_select]);
                ui_state = UI_SETTINGS; drawSettings();
            }
            break;

        case UI_KEYBOARD:
            break;
    }
}

//==============================================================================
// タッチ処理
//==============================================================================
static const char* uiShort() {
    switch (ui_state) {
        case UI_LIST: return "list"; case UI_DETAIL: return "detail"; case UI_PLAYING: return "playing";
        case UI_SETTINGS: return "settings"; case UI_KEYBOARD: return "kbd"; default: return "sel";
    }
}

int findEventById(const char* id) {
    if (!id || !id[0]) return -1;
    for (int i = 0; i < event_count; i++) if (strcmp(events[i].id, id) == 0) return i;
    return -1;
}

void handleTouch(int tx, int ty) {
    last_interaction_ms = millis();
    uiEventPush("touch", tx, ty, uiShort());

    // 左上タッチ → スクリーンショット (ホストへ送信)
    if (tx < 80 && ty < 80) {
        saveScreenshot();
        return;
    }

    switch (ui_state) {
        case UI_LIST: {
            // ボタンチェック
            if (ty >= btn_prev.y0 && ty <= btn_prev.y1) {
                if (tx >= btn_prev.x0 && tx <= btn_prev.x1) {
                    // 前日
                    if (page_start > 0) {
                        struct tm cur_tm; localtime_r(&events[page_start].start, &cur_tm);
                        int cur_day = cur_tm.tm_mday + cur_tm.tm_mon * 100 + cur_tm.tm_year * 10000;
                        int found = -1;
                        for (int i = page_start - 1; i >= 0; i--) {
                            struct tm t; localtime_r(&events[i].start, &t);
                            int day = t.tm_mday + t.tm_mon * 100 + t.tm_year * 10000;
                            if (day < cur_day) {
                                found = i;
                                for (int j = i - 1; j >= 0; j--) {
                                    struct tm t2; localtime_r(&events[j].start, &t2);
                                    int day2 = t2.tm_mday + t2.tm_mon * 100 + t2.tm_year * 10000;
                                    if (day2 == day) found = j; else break;
                                }
                                break;
                            }
                        }
                        if (found >= 0) { page_start = found; selected_event = found; }
                        else { page_start = 0; selected_event = 0; }
                        drawList();
                    }
                    return;
                } else if (tx >= btn_next.x0 && tx <= btn_next.x1) {
                    // 翌日
                    if (page_start < event_count - 1) {
                        struct tm cur_tm; localtime_r(&events[page_start].start, &cur_tm);
                        int cur_day = cur_tm.tm_mday + cur_tm.tm_mon * 100 + cur_tm.tm_year * 10000;
                        for (int i = page_start + 1; i < event_count; i++) {
                            struct tm t; localtime_r(&events[i].start, &t);
                            int day = t.tm_mday + t.tm_mon * 100 + t.tm_year * 10000;
                            if (day > cur_day) { page_start = i; selected_event = i; break; }
                        }
                        drawList();
                    }
                    return;
                } else if (tx >= btn_today.x0 && tx <= btn_today.x1) {
                    // 今日
                    time_t now = time(nullptr); struct tm now_tm; localtime_r(&now, &now_tm);
                    int today = now_tm.tm_mday + now_tm.tm_mon * 100 + now_tm.tm_year * 10000;
                    for (int i = 0; i < event_count; i++) {
                        struct tm t; localtime_r(&events[i].start, &t);
                        int day = t.tm_mday + t.tm_mon * 100 + t.tm_year * 10000;
                        if (day >= today) { page_start = i; selected_event = i; break; }
                    }
                    drawList();
                    return;
                } else if (tx >= btn_detail.x0 && tx <= btn_detail.x1) {
                    // 詳細
                    if (selected_event >= 0 && selected_event < event_count) {
                        ui_state = UI_DETAIL; detail_scroll = 0; drawDetail(selected_event);
                    }
                    return;
                }
            }

            // 日付ヘッダー無視
            for (int i = 0; i < date_header_count; i++) {
                if (ty >= date_header_y0[i] && ty <= date_header_y1[i]) return;
            }

            // 予定行タップ → 詳細
            for (int i = 0; i < displayed_count; i++) {
                if (ty >= row_y0[i] && ty <= row_y1[i]) {
                    selected_event = row_event_idx[i];
                    ui_state = UI_DETAIL; detail_scroll = 0;
                    uiEventPush("ui", 0, 0, events[selected_event].id);
                    drawDetail(selected_event);
                    return;
                }
            }
            break;
        }

        case UI_DETAIL:
            ui_state = UI_LIST; waitEPDReady(); drawList(false, false, false, true); break;

        case UI_PLAYING:
            finishAlarm(); break;

        case UI_KEYBOARD: {
            int hit = getKeyboardHit(tx, ty);
            processKeyboardHit(hit);
            break;
        }

        case UI_MIDI_SELECT:
        case UI_BAUD_SELECT:
        case UI_PORT_SELECT:
            ui_state = UI_SETTINGS; drawSettings(); break;

        case UI_SETTINGS:
            // ナビゲーションボタン
            if (ty >= 900 && ty < 948) {
                if (tx >= 5 && tx < 135) { settings_cursor = 0; drawSettings(true); return; }
                if (tx >= 145 && tx < 275) { settings_cursor = SET_COUNT - 1; drawSettings(true); return; }
                if (tx >= 285 && tx < 415) { ui_state = UI_LIST; scrollToToday(); drawList(); return; }
            }
            // 項目タップ
            {
                int itemY = 46, rowH = 60;
                int maxVisible = (895 - itemY) / rowH;
                for (int n = 0; n < maxVisible; n++) {
                    int i = settings_cursor + n;
                    if (i >= SET_COUNT) break;
                    if (ty >= itemY && ty < itemY + rowH) {
                        settings_cursor = i; handleSettingsSelect(); return;
                    }
                    itemY += rowH;
                }
            }
            break;
    }
}

//==============================================================================
// アラームチェック
//   テーブル(alarm_time/triggered)はホスト由来。端末は時刻になったら鳴らし、
//   完了を ACK する。ホストに届かなければ ACK を保持し再送する (sync_client)。
//==============================================================================
void checkAlarms() {
    if (midi_playing) return;
    if (!time_valid) return;          // ホストから時刻をもらうまで鳴らさない

    time_t now = time(nullptr);

    // 毎分デバッグ出力
    if (now - last_alarm_debug >= 60) {
        last_alarm_debug = now;
        struct tm lt; localtime_r(&now, &lt);
        int pending = 0;
        for (int i = 0; i < event_count; i++)
            for (int k = 0; k < events[i].alarm_count; k++) if (!events[i].triggered[k]) pending++;
        Serial.printf("=== ALARM CHECK [%02d/%02d %02d:%02d:%02d] ver.%s ev=%d pending=%d heap=%u host=%s rev=%ld/%ld ===\n",
                      lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, BUILD_VERSION,
                      event_count, pending, ESP.getFreeHeap(), host_online ? "up" : "DOWN", local_rev, host_rev);
    }

    for (int i = 0; i < event_count; i++) {
        if (!events[i].has_alarm) continue;
        int fireSlot = -1;
        for (int k = 0; k < events[i].alarm_count; k++) {
            if (!events[i].triggered[k] && events[i].alarm_time[k] <= now) { fireSlot = k; break; }
        }
        if (fireSlot < 0) continue;

        // 大幅に過去のもの(ホストが expired にしそこねた等)は鳴らさず ACK だけ返す
        if (now - events[i].alarm_time[fireSlot] > 3600) {
            char aid[40]; alarmIdOf(events[i], fireSlot, aid, sizeof(aid));
            events[i].triggered[fireSlot] = true;
            events_cache_dirty = true;
            logLine("skip stale alarm %s late=%lds", aid, (long)(now - events[i].alarm_time[fireSlot]));
            ackAlarm(aid, "stale");
            continue;
        }

        char aid[40]; alarmIdOf(events[i], fireSlot, aid, sizeof(aid));
        Serial.printf("\n*** ALARM FIRING *** %s '%s' (off=%dmin)\n", aid, events[i].summary(), events[i].offset_min[fireSlot]);
        logLine("ALARM-FIRE %s late=%lds '%.40s'", aid, (long)(now - events[i].alarm_time[fireSlot]), events[i].summary());

        playing_event = i;
        playing_alarm_idx = fireSlot;
        strlcpy(playing_alarm_id, aid, sizeof(playing_alarm_id));
        play_file_override[0] = '\0';

        int dur = events[i].play_duration_sec;
        if (dur < 0) dur = config.play_duration;
        play_duration_ms = dur * 1000;
        int rep = events[i].play_repeat;
        if (rep < 0) rep = config.play_repeat;
        if (rep < 1) rep = 1;
        play_repeat_remaining = rep;

        String midiPath = getMidiPath(i);
        play_start_ms = millis();
        if (startMidiPlayback(midiPath.c_str())) {
            ui_state = UI_PLAYING;
            drawPlaying(i);
            uiEventPush("alarm", 0, 0, aid);
            sendHeartbeat(true);      // ホストへ即「鳴動中」を報告
        } else {
            events[i].triggered[fireSlot] = true;
            events_cache_dirty = true;
            playing_event = -1; playing_alarm_idx = -1; playing_alarm_id[0] = '\0';
            ackAlarm(aid, "failed");
        }
        break;
    }
}
