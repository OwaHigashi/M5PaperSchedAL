#include "globals.h"
#include <SD.h>
#include <time.h>

//==============================================================================
// スイッチ処理
//==============================================================================
void checkSwitches() {
    bool sw_l = digitalRead(SW_L_PIN);
    bool sw_r = digitalRead(SW_R_PIN);
    bool sw_p = digitalRead(SW_P_PIN);

    if (!sw_l && sw_l_prev) { Serial.println("SW_L pressed"); last_interaction_ms = millis(); handleSwitch('L'); }
    if (!sw_r && sw_r_prev) { Serial.println("SW_R pressed"); last_interaction_ms = millis(); handleSwitch('R'); }
    if (!sw_p && sw_p_prev) { Serial.println("SW_P pressed"); last_interaction_ms = millis(); handleSwitch('P'); }

    sw_l_prev = sw_l;
    sw_r_prev = sw_r;
    sw_p_prev = sw_p;
}

void handleSwitch(char sw) {
    switch (ui_state) {
        case UI_LIST:
            if (sw == 'P') {
                ui_state = UI_SETTINGS; settings_cursor = 0; drawSettings();
            } else if (sw == 'L') {
                if (event_count > 0) {
                    if (selected_event > 0) {
                        selected_event--;
                        if (selected_event < page_start) page_start = max(0, page_start - ITEMS_PER_PAGE);
                    } else {
                        selected_event = event_count - 1;
                        page_start = max(0, event_count - ITEMS_PER_PAGE);
                    }
                    drawList();
                }
            } else if (sw == 'R') {
                if (event_count > 0) {
                    if (selected_event < event_count - 1) {
                        selected_event++;
                        if (selected_event >= page_start + ITEMS_PER_PAGE) page_start += ITEMS_PER_PAGE;
                    } else {
                        selected_event = 0; page_start = 0;
                    }
                    drawList();
                }
            }
            break;

        case UI_DETAIL:
            if (sw == 'P') { ui_state = UI_LIST; drawList(); }
            else if (sw == 'L') { if (detail_scroll > 0) { detail_scroll--; drawDetail(selected_event); } }
            else if (sw == 'R') { detail_scroll++; drawDetail(selected_event); }
            break;

        case UI_PLAYING:
            break;

        case UI_SETTINGS:
            if (sw == 'L') {
                if (settings_cursor > 0) { settings_cursor--; drawSettings(); }
                else { ui_state = UI_LIST; scrollToToday(); drawList(); }
            } else if (sw == 'R') {
                settings_cursor = (settings_cursor + 1) % SET_COUNT; drawSettings();
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
void handleTouch(int tx, int ty) {
    last_interaction_ms = millis();

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
                    drawDetail(selected_event);
                    return;
                }
            }
            break;
        }

        case UI_DETAIL:
            ui_state = UI_LIST; drawList(); break;

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
                if (tx >= 5 && tx < 135) { settings_cursor = 0; drawSettings(); return; }
                if (tx >= 145 && tx < 275) { settings_cursor = SET_COUNT - 1; drawSettings(); return; }
                if (tx >= 285 && tx < 415) { ui_state = UI_LIST; scrollToToday(); drawList(); return; }
            }
            // 項目タップ
            {
                int itemY = 45, rowH = 50;
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
//==============================================================================
void checkAlarms() {
    if (midi_playing) return;

    time_t now = time(nullptr);

    // 毎分デバッグ出力
    if (now - last_alarm_debug >= 60) {
        last_alarm_debug = now;
        struct tm lt; localtime_r(&now, &lt);
        Serial.printf("\n=== ALARM CHECK [%02d/%02d %02d:%02d:%02d] ver.%s heap:%d sd:%s ===\n",
                      lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
                      BUILD_VERSION, ESP.getFreeHeap(), sd_healthy ? "OK" : "NG");

        int pending = 0;
        for (int i = 0; i < event_count; i++) {
            if (!events[i].has_alarm || events[i].triggered) continue;
            pending++;
            struct tm at, st;
            localtime_r(&events[i].alarm_time, &at);
            localtime_r(&events[i].start, &st);
            long remain = (long)(events[i].alarm_time - now);
            Serial.printf("  [%d] %s\n", i, events[i].summary.c_str());
            Serial.printf("      event:%02d/%02d %02d:%02d  alarm:%02d/%02d %02d:%02d  off:%dmin  remain:%lds\n",
                          st.tm_mon+1, st.tm_mday, st.tm_hour, st.tm_min,
                          at.tm_mon+1, at.tm_mday, at.tm_hour, at.tm_min,
                          events[i].offset_min, remain);
            if (events[i].midi_file.length() > 0)
                Serial.printf("      midi:%s (%s)\n", events[i].midi_file.c_str(), events[i].midi_is_url ? "URL" : "SD");
        }
        if (pending == 0) Serial.println("  (no pending alarms)");
        Serial.printf("=== events:%d, pending:%d, heap:%d, WiFi:%d, fails:%d ===\n\n",
                      event_count, pending, ESP.getFreeHeap(), WiFi.RSSI(), fetch_fail_count);
    }

    // アラーム発火チェック
    for (int i = 0; i < event_count; i++) {
        if (events[i].has_alarm && !events[i].triggered && events[i].alarm_time <= now) {
            Serial.printf("\n*** ALARM FIRING! ***\n");
            Serial.printf("  Event: %s\n", events[i].summary.c_str());

            // ntfy通知
            {
                struct tm st; localtime_r(&events[i].start, &st);
                char timeStr[32];
                snprintf(timeStr, sizeof(timeStr), "%02d:%02d", st.tm_hour, st.tm_min);
                String notifyMsg = String(timeStr) + " " + events[i].summary;
                sendNtfyNotification("M5Paper Alarm", notifyMsg);
            }

            playing_event = i;

            int dur = events[i].play_duration_sec;
            if (dur < 0) dur = config.play_duration;
            play_duration_ms = dur * 1000;

            int rep = events[i].play_repeat;
            if (rep < 0) rep = config.play_repeat;
            if (rep < 1) rep = 1;
            play_repeat_remaining = rep;

            Serial.printf("  Duration: %s, Repeat: %d\n",
                          dur == 0 ? "1song" : (String(dur) + "sec").c_str(), play_repeat_remaining);

            String midiPath = getMidiPath(i);
            waitEPDReady();
            Serial.printf("  MIDI: %s (exists:%s)\n", midiPath.c_str(), SD.exists(midiPath.c_str()) ? "Y" : "N");

            play_start_ms = millis();
            if (startMidiPlayback(midiPath.c_str())) {
                ui_state = UI_PLAYING;
                drawPlaying(i);
            } else {
                events[i].triggered = true;
            }
            break;
        }
    }
}
