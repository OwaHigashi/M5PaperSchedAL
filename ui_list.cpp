#include "globals.h"
#include "ui_colors.h"
#include <time.h>

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
    page_start = 0;
    selected_event = 0;
}

// 1行分のイベント描画（drawListとupdateListCursorから共通利用）
static void drawEventRow(int evtIdx, int y, bool highlighted, int nextEventIdx) {
    int rowH = 60;
    canvas.setTextSize(26);
    canvas.setTextColor(COL_ROW_TEXT);

    struct tm st;
    localtime_r(&events[evtIdx].start, &st);

    String timeStr;
    if (events[evtIdx].is_allday) {
        timeStr = "[終日]";
    } else {
        timeStr = formatTime(st.tm_hour, st.tm_min);
    }

    String alarmMark = "";
    bool showAlarmMark = false;
    if (events[evtIdx].has_alarm) {
        alarmMark = events[evtIdx].triggered ? "*" : "♪";
        showAlarmMark = !events[evtIdx].triggered;
    }

    String summary = removeUnsupportedChars(events[evtIdx].summary());
    int maxWidth = config.text_wrap ? 26 : 30;
    String dispSummary = utf8Substring(summary, maxWidth);

    const int TIME_X = 10;
    const int MARK_X = 90;
    const int SUMMARY_X = 118;

    if (config.text_wrap && summary.length() > dispSummary.length()) {
        drawText(timeStr, TIME_X, y + 3);
        if (showAlarmMark) {
            canvas.fillRect(MARK_X - 2, y + 1, 26, 28, COL_ALARM_BADGE_BG);
            canvas.setTextColor(COL_ALARM_BADGE_FG);
            drawText(alarmMark, MARK_X, y + 3);
            canvas.setTextColor(COL_ROW_TEXT);
        } else if (alarmMark.length() > 0) {
            drawText(alarmMark, MARK_X, y + 3);
        }
        drawText(dispSummary, SUMMARY_X, y + 3);
        canvas.setTextSize(22);
        String rest = summary.substring(dispSummary.length());
        String line2 = utf8Substring(rest, 34);
        drawText(line2, 85, y + 32);
    } else {
        drawText(timeStr, TIME_X, y + 16);
        if (showAlarmMark) {
            canvas.fillRect(MARK_X - 2, y + 14, 26, 28, COL_ALARM_BADGE_BG);
            canvas.setTextColor(COL_ALARM_BADGE_FG);
            drawText(alarmMark, MARK_X, y + 16);
            canvas.setTextColor(COL_ROW_TEXT);
        } else if (alarmMark.length() > 0) {
            drawText(alarmMark, MARK_X, y + 16);
        }
        String dispPart = dispSummary;
        if (summary.length() > dispSummary.length()) dispPart += "..";
        drawText(dispPart, SUMMARY_X, y + 16);
    }

    if (evtIdx == nextEventIdx) {
        canvas.drawLine(10, y + rowH - 5, 530, y + rowH - 5, COL_NEXT_EVENT_LINE);
        canvas.drawLine(10, y + rowH - 4, 530, y + rowH - 4, COL_NEXT_EVENT_LINE);
    }

    canvas.setTextColor(COL_TEXT);
}

// カーソル移動のみの軽量更新（2行+フッタのみ再描画）
void updateListCursor(int old_sel, int new_sel) {
    unsigned long t0 = millis();
    int rowH = 60;

    // キャンバス状態を初期化
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(COL_TEXT);

    // 表示行の中から旧・新カーソルを探す
    int old_d = -1, new_d = -1;
    for (int d = 0; d < displayed_count; d++) {
        if (row_event_idx[d] == old_sel) old_d = d;
        if (row_event_idx[d] == new_sel) new_d = d;
    }

    Serial.printf("[CURSOR] old_sel=%d(d=%d) new_sel=%d(d=%d) displayed=%d\n",
                  old_sel, old_d, new_sel, new_d, displayed_count);

    // 画面外ならpage_startを調整してフル再描画
    if (old_d < 0 || new_d < 0) {
        Serial.println("[CURSOR] fallback to full drawList");
        // selected_eventが表示範囲に入るようpage_startを調整
        page_start = max(0, new_sel - 2);  // 少し上に余裕を持たせる
        drawList();  // ページ遷移 → GC16
        return;
    }

    // nextEventIdx を求める
    time_t now = time(nullptr);
    int nextEventIdx = -1;
    time_t nextEventTime = 0x7FFFFFFF;
    for (int i = 0; i < event_count; i++) {
        if (!events[i].is_allday && events[i].start > now && events[i].start < nextEventTime) {
            nextEventTime = events[i].start;
            nextEventIdx = i;
        }
    }

    // 旧カーソル行: ハイライト除去
    canvas.fillRect(0, row_y0[old_d], 540, rowH, COL_BG);
    drawEventRow(old_sel, row_y0[old_d], false, nextEventIdx);

    // 新カーソル行: ハイライト設定
    canvas.fillRect(0, row_y0[new_d], 540, rowH, COL_CURSOR_BG);
    drawEventRow(new_sel, row_y0[new_d], true, nextEventIdx);

    // フッタのページ情報を更新
    canvas.fillRect(0, 855, 200, 30, COL_BG);
    canvas.setTextSize(20);
    canvas.setTextColor(COL_FOOTER_TEXT);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d件", new_sel + 1, event_count);
    drawText(buf, 10, 860);

    unsigned long draw_ms = millis() - t0;
    unsigned long t1 = millis();
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    Serial.printf("[CURSOR] draw:%lums push:%lums total:%lums\n",
                  draw_ms, millis() - t1, millis() - t0);
}

void drawList(bool fast, bool skip_push, bool highlight_changes) {
    canvas.fillCanvas(COL_BG);
    canvas.setTextColor(COL_HEADER_TEXT);
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

    // WiFi・SD状態
    canvas.setTextSize(22);
    String status = (WiFi.status() == WL_CONNECTED) ? "WiFi:OK" : "WiFi:NG";
    if (!sd_healthy) status += " SD:NG";
    drawText(status, 400, 10);

    int y = 45;
    int rowH = 60;
    int lastDay = -1;
    date_header_count = 0;
    int displayed = 0;
    int listBottom = 850;

    // 「次の予定」を見つける
    int nextEventIdx = -1;
    time_t nextEventTime = 0x7FFFFFFF;
    for (int i = 0; i < event_count; i++) {
        if (!events[i].is_allday && events[i].start > now) {
            if (events[i].start < nextEventTime) {
                nextEventTime = events[i].start;
                nextEventIdx = i;
            }
        }
    }

    if (event_count == 0) {
        canvas.setTextSize(28);
        canvas.setTextDatum(MC_DATUM);
        drawText("予定がありません", 270, 200);
        canvas.setTextSize(22);
        if (WiFi.status() != WL_CONNECTED) {
            drawText("WiFi未接続", 270, 260);
        } else if (fetch_fail_count >= 3) {
            char msg[96];
            int backoff_min = min((fetch_fail_count - 2) * 5, 30);
            snprintf(msg, sizeof(msg), "サーバ接続不可 (%d回失敗)", fetch_fail_count);
            drawText(msg, 270, 250);
            snprintf(msg, sizeof(msg), "次回再試行: %d分後", backoff_min);
            drawText(msg, 270, 280);
        } else if (fetch_fail_count > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "ICS取得失敗 (%d回)", fetch_fail_count);
            drawText(msg, 270, 260);
        }
        drawText("P長押し → 設定", 270, 330);
        canvas.setTextDatum(TL_DATUM);
    }

    // ★ 今日の日付が表示範囲に含まれない場合、空ヘッダーを挿入
    //    例: 今日2/15に予定なし → page_startが2/16のイベント → 2/15が見えない
    {
        struct tm now_tm;
        localtime_r(&now, &now_tm);
        int today = now_tm.tm_mday + now_tm.tm_mon * 100 + now_tm.tm_year * 10000;

        // page_startの日付を取得
        bool today_has_events = false;
        if (page_start < event_count) {
            struct tm ps_tm;
            localtime_r(&events[page_start].start, &ps_tm);
            int ps_day = ps_tm.tm_mday + ps_tm.tm_mon * 100 + ps_tm.tm_year * 10000;
            today_has_events = (ps_day == today);
        }

        // 今日のイベントが無く、page_startが今日以降を指している場合
        if (!today_has_events && page_start > 0) {
            // page_startの前のイベントが今日より前 = 今日は空
            struct tm prev_tm;
            localtime_r(&events[page_start - 1].start, &prev_tm);
            int prev_day = prev_tm.tm_mday + prev_tm.tm_mon * 100 + prev_tm.tm_year * 10000;
            if (prev_day < today) {
                // 今日の空ヘッダーを描画（薄めのスタイル）
                canvas.fillRect(0, y, 540, 38, COL_DATE_EMPTY_BG);
                canvas.setTextSize(28);
                snprintf(buf, sizeof(buf), "── %d/%d (%s) ──",
                         now_tm.tm_mon + 1, now_tm.tm_mday,
                         (now_tm.tm_wday == 0) ? "日" : (now_tm.tm_wday == 1) ? "月" :
                         (now_tm.tm_wday == 2) ? "火" : (now_tm.tm_wday == 3) ? "水" :
                         (now_tm.tm_wday == 4) ? "木" : (now_tm.tm_wday == 5) ? "金" : "土");
                canvas.setTextColor(COL_DATE_EMPTY_TEXT);
                drawText(buf, 120, y + 5);
                // 太字なし（通常は2回描画で太字化）
                canvas.setTextColor(COL_TEXT);
                if (date_header_count < 10) {
                    date_header_y0[date_header_count] = y;
                    date_header_y1[date_header_count] = y + 38;
                    date_header_count++;
                }
                Serial.printf("[LIST] date header at y=%d: %s (today, no events)\n", y, buf);
                y += 42;

                // 「予定なし」表示
                canvas.setTextSize(24);
                canvas.setTextColor(COL_NO_EVENT_TEXT);
                drawText("予定なし", 200, y + 8);
                canvas.setTextColor(COL_TEXT);
                y += 42;
                lastDay = today;
            }
        }
        // page_start == 0 で今日のイベントがない場合（全イベントが今日より後）
        else if (!today_has_events && page_start == 0 && event_count > 0) {
            struct tm first_tm;
            localtime_r(&events[0].start, &first_tm);
            int first_day = first_tm.tm_mday + first_tm.tm_mon * 100 + first_tm.tm_year * 10000;
            if (first_day > today) {
                canvas.fillRect(0, y, 540, 38, COL_DATE_EMPTY_BG);
                canvas.setTextSize(28);
                snprintf(buf, sizeof(buf), "── %d/%d (%s) ──",
                         now_tm.tm_mon + 1, now_tm.tm_mday,
                         (now_tm.tm_wday == 0) ? "日" : (now_tm.tm_wday == 1) ? "月" :
                         (now_tm.tm_wday == 2) ? "火" : (now_tm.tm_wday == 3) ? "水" :
                         (now_tm.tm_wday == 4) ? "木" : (now_tm.tm_wday == 5) ? "金" : "土");
                canvas.setTextColor(COL_DATE_EMPTY_TEXT);
                drawText(buf, 120, y + 5);
                canvas.setTextColor(COL_TEXT);
                if (date_header_count < 10) {
                    date_header_y0[date_header_count] = y;
                    date_header_y1[date_header_count] = y + 38;
                    date_header_count++;
                }
                Serial.printf("[LIST] date header at y=%d: %s (today, no events)\n", y, buf);
                y += 42;

                canvas.setTextSize(24);
                canvas.setTextColor(COL_NO_EVENT_TEXT);
                drawText("予定なし", 200, y + 8);
                canvas.setTextColor(COL_TEXT);
                y += 42;
                lastDay = today;
            }
        }
    }

    for (int i = page_start; i < event_count && y < listBottom; i++) {
        struct tm st;
        localtime_r(&events[i].start, &st);

        // 日付ヘッダー
        int thisDay = st.tm_mday + st.tm_mon * 100 + st.tm_year * 10000;
        if (thisDay != lastDay && date_header_count < 10) {
            if (y + 38 + rowH > listBottom) break;

            canvas.fillRect(0, y, 540, 38, COL_DATE_BG);
            canvas.setTextSize(28);
            snprintf(buf, sizeof(buf), "── %d/%d (%s) ──",
                     st.tm_mon + 1, st.tm_mday,
                     (st.tm_wday == 0) ? "日" : (st.tm_wday == 1) ? "月" :
                     (st.tm_wday == 2) ? "火" : (st.tm_wday == 3) ? "水" :
                     (st.tm_wday == 4) ? "木" : (st.tm_wday == 5) ? "金" : "土");
            canvas.setTextColor(COL_DATE_TEXT);
            drawText(buf, 120, y + 5);
            drawText(buf, 121, y + 5);
            canvas.setTextColor(COL_TEXT);
            date_header_y0[date_header_count] = y;
            date_header_y1[date_header_count] = y + 38;
            date_header_count++;
            y += 42;
            lastDay = thisDay;
            Serial.printf("[LIST] date header at y=%d: %s\n", y - 42, buf);
        }

        if (y + rowH > listBottom) break;

        row_y0[displayed] = y;
        row_y1[displayed] = y + rowH;
        row_event_idx[displayed] = i;

        bool hl = (i == selected_event);
        bool changed = highlight_changes && displayed < MAX_DISPLAY_ROWS && row_changed[displayed];
        if (hl) {
            canvas.fillRect(0, y, 540, rowH, COL_CURSOR_BG);
        } else if (changed) {
            canvas.fillRect(0, y, 540, rowH, COL_CHANGED_BG);
        }

        // デバッグ: 最初の3行 + 変更行を出力
        if (displayed < 3 || changed) {
            struct tm dbg_st;
            localtime_r(&events[i].start, &dbg_st);
            String dbg_time = events[i].is_allday ? "[終日]" : formatTime(dbg_st.tm_hour, dbg_st.tm_min);
            String dbg_sum = removeUnsupportedChars(events[i].summary());
            Serial.printf("[LIST] row %d: y=%d time='%s' sum='%s' (len=%d)%s\n",
                          displayed, y, dbg_time.c_str(), dbg_sum.c_str(), dbg_sum.length(),
                          changed ? " [CHANGED]" : "");
        }

        drawEventRow(i, y, hl, nextEventIdx);

        displayed++;
        y += rowH;
    }
    displayed_count = displayed;

    // 次のアラーム表示
    canvas.setTextSize(22);
    int nextIdx = -1;
    for (int i = 0; i < event_count; i++) {
        if (events[i].has_alarm && !events[i].triggered && events[i].alarm_time > now) {
            nextIdx = i; break;
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

    // ボタン描画
    int btnY = 900;
    int btnH = 50;
    canvas.setTextSize(22);

    btn_prev = {5, btnY, 125, btnY + btnH};
    canvas.drawRect(btn_prev.x0, btn_prev.y0, 120, btnH, COL_BTN_BORDER);
    canvas.drawString("<前日", 30, btnY + 14);

    btn_next = {130, btnY, 255, btnY + btnH};
    canvas.drawRect(btn_next.x0, btn_next.y0, 125, btnH, COL_BTN_BORDER);
    canvas.drawString("翌日>", 160, btnY + 14);

    btn_today = {260, btnY, 385, btnY + btnH};
    canvas.drawRect(btn_today.x0, btn_today.y0, 125, btnH, COL_BTN_BORDER);
    canvas.drawString("今日", 295, btnY + 14);

    btn_detail = {390, btnY, 535, btnY + btnH};
    canvas.drawRect(btn_detail.x0, btn_detail.y0, 145, btnH, COL_BTN_BORDER);
    canvas.drawString("詳細", 435, btnY + 14);

    Serial.printf("[LIST] push: displayed=%d events, y_final=%d mode=%s\n",
                  displayed_count, y, skip_push ? "SKIP" : (fast ? "DU4" : "GC16"));
    if (!skip_push) {
        unsigned long t0 = millis();
        canvas.pushCanvas(0, 0, fast ? UPDATE_MODE_DU4 : UPDATE_MODE_GC16);
        Serial.printf("[LIST] pushCanvas took %lu ms\n", millis() - t0);
    }
    // 表示内容スナップショット保存（pushの有無に関わらず内部状態を同期）
    saveDisplaySnapshot();
}
