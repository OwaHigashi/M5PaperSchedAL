#include "globals.h"
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
        } else if (fetch_fail_count > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "ICS取得失敗 (%d回)", fetch_fail_count);
            drawText(msg, 270, 260);
        }
        drawText("P長押し → 設定", 270, 310);
        canvas.setTextDatum(TL_DATUM);
    }

    for (int i = page_start; i < event_count && y < listBottom; i++) {
        struct tm st;
        localtime_r(&events[i].start, &st);

        // 日付ヘッダー
        int thisDay = st.tm_mday + st.tm_mon * 100 + st.tm_year * 10000;
        if (thisDay != lastDay && date_header_count < 10) {
            if (y + 38 + rowH > listBottom) break;

            canvas.fillRect(0, y, 540, 38, 12);
            canvas.setTextSize(28);
            snprintf(buf, sizeof(buf), "── %d/%d (%s) ──",
                     st.tm_mon + 1, st.tm_mday,
                     (st.tm_wday == 0) ? "日" : (st.tm_wday == 1) ? "月" :
                     (st.tm_wday == 2) ? "火" : (st.tm_wday == 3) ? "水" :
                     (st.tm_wday == 4) ? "木" : (st.tm_wday == 5) ? "金" : "土");
            canvas.setTextColor(0);
            drawText(buf, 120, y + 5);
            drawText(buf, 121, y + 5);
            canvas.setTextColor(15);
            date_header_y0[date_header_count] = y;
            date_header_y1[date_header_count] = y + 38;
            date_header_count++;
            y += 42;
            lastDay = thisDay;
        }

        if (y + rowH > listBottom) break;

        row_y0[displayed] = y;
        row_y1[displayed] = y + rowH;
        row_event_idx[displayed] = i;

        if (i == selected_event) {
            canvas.fillRect(0, y, 540, rowH, 3);
        }

        canvas.setTextSize(26);

        String timeStr;
        if (events[i].is_allday) {
            timeStr = "[終日]";
        } else {
            timeStr = formatTime(st.tm_hour, st.tm_min);
        }

        String alarmMark = "";
        bool showAlarmMark = false;
        if (events[i].has_alarm) {
            alarmMark = events[i].triggered ? "*" : "♪";
            showAlarmMark = !events[i].triggered;
        }

        String summary = removeUnsupportedChars(events[i].summary);
        int maxWidth = config.text_wrap ? 26 : 30;
        String dispSummary = utf8Substring(summary, maxWidth);

        const int TIME_X = 10;
        const int MARK_X = 90;
        const int SUMMARY_X = 118;

        if (config.text_wrap && summary.length() > dispSummary.length()) {
            drawText(timeStr, TIME_X, y + 3);
            if (showAlarmMark) {
                canvas.fillRect(MARK_X - 2, y + 1, 26, 28, 15);
                canvas.setTextColor(0);
                drawText(alarmMark, MARK_X, y + 3);
                canvas.setTextColor(15);
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
                canvas.fillRect(MARK_X - 2, y + 14, 26, 28, 15);
                canvas.setTextColor(0);
                drawText(alarmMark, MARK_X, y + 16);
                canvas.setTextColor(15);
            } else if (alarmMark.length() > 0) {
                drawText(alarmMark, MARK_X, y + 16);
            }
            String dispPart = dispSummary;
            if (summary.length() > dispSummary.length()) dispPart += "..";
            drawText(dispPart, SUMMARY_X, y + 16);
        }

        if (i == nextEventIdx) {
            canvas.drawLine(10, y + rowH - 5, 530, y + rowH - 5, 15);
            canvas.drawLine(10, y + rowH - 4, 530, y + rowH - 4, 15);
        }

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
    canvas.drawRect(btn_prev.x0, btn_prev.y0, 120, btnH, 15);
    canvas.drawString("<前日", 30, btnY + 14);

    btn_next = {130, btnY, 255, btnY + btnH};
    canvas.drawRect(btn_next.x0, btn_next.y0, 125, btnH, 15);
    canvas.drawString("翌日>", 160, btnY + 14);

    btn_today = {260, btnY, 385, btnY + btnH};
    canvas.drawRect(btn_today.x0, btn_today.y0, 125, btnH, 15);
    canvas.drawString("今日", 295, btnY + 14);

    btn_detail = {390, btnY, 535, btnY + btnH};
    canvas.drawRect(btn_detail.x0, btn_detail.y0, 145, btnH, 15);
    canvas.drawString("詳細", 435, btnY + 14);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}
