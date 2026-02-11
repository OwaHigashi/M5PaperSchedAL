#include "globals.h"
#include <time.h>

void drawDetail(int idx) {
    if (idx < 0 || idx >= event_count) return;

    canvas.fillCanvas(0);
    canvas.setTextColor(15);
    canvas.setTextDatum(TL_DATUM);

    EventItem& e = events[idx];
    struct tm st;
    localtime_r(&e.start, &st);

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

    canvas.setTextSize(22);
    if (e.has_alarm) {
        struct tm al;
        localtime_r(&e.alarm_time, &al);
        String alTime = formatTime(al.tm_hour, al.tm_min);
        String offsetStr;
        if (e.offset_min > 0)       offsetStr = String(e.offset_min) + "分前";
        else if (e.offset_min < 0)  offsetStr = String(-e.offset_min) + "分後";
        else                         offsetStr = "時刻通り";
        snprintf(buf, sizeof(buf), "アラーム: %s (%s) %s",
                 alTime.c_str(), offsetStr.c_str(), e.triggered ? "[済]" : "");
        drawText(buf, 10, 45);

        canvas.setTextSize(18);
        String extraInfo = "";
        if (e.midi_file.length() > 0) {
            extraInfo += e.midi_is_url ? "♪URL:" : "♪SD:";
            extraInfo += e.midi_file;
        }
        int dur = e.play_duration_sec >= 0 ? e.play_duration_sec : config.play_duration;
        if (extraInfo.length() > 0) extraInfo += " ";
        extraInfo += dur == 0 ? "1曲" : String(dur) + "秒";
        int rep = e.play_repeat >= 0 ? e.play_repeat : config.play_repeat;
        extraInfo += " x" + String(rep);
        drawText(extraInfo, 10, 72);
    } else {
        drawText("アラーム: なし", 10, 45);
    }

    // SUMMARY（複数行）
    canvas.setTextSize(28);
    int y = 100;
    String summary = removeUnsupportedChars(e.summary);
    int summaryWidth = 36;
    while (summary.length() > 0 && y < 200) {
        String line = utf8Substring(summary, summaryWidth);
        drawText(line, 10, y);
        summary = summary.substring(line.length());
        y += 36;
    }

    // DESCRIPTION（折り返し表示、スクロール対応）
    canvas.setTextSize(22);
    y = max(y + 10, 210);
    int maxY = 890;
    String desc = removeUnsupportedChars(e.description);
    int skipLines = detail_scroll;
    int lineHeight = 28;
    int lineWidth = 42;

    while (desc.length() > 0) {
        String line = utf8Substring(desc, lineWidth);
        if (skipLines > 0) {
            skipLines--;
        } else if (y < maxY) {
            drawText(line, 10, y);
            y += lineHeight;
        }
        if (line.length() == 0) break;
        desc = desc.substring(line.length());
    }

    canvas.setTextSize(22);
    drawText("タップ:戻る  L/R:スクロール", 10, 910);
    if (detail_scroll > 0) drawText("↑", 500, 200);
    if (y >= maxY && desc.length() > 0) drawText("↓", 500, 850);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawPlaying(int idx) {
    if (idx < 0 || idx >= event_count) return;

    canvas.fillCanvas(0);
    canvas.setTextColor(15);
    canvas.setTextDatum(MC_DATUM);

    EventItem& e = events[idx];

    canvas.setTextSize(48);
    canvas.drawString("ALARM!", 270, 60);

    canvas.setTextSize(32);
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

    canvas.setTextSize(22);
    String info = "";
    if (play_duration_ms > 0) info += String(play_duration_ms / 1000) + "秒";
    else info += "1曲";
    info += " x" + String(play_repeat_remaining) + "回";
    canvas.drawString(info, 270, 320);

    // DESCRIPTION
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(24);
    String desc = removeUnsupportedChars(e.description);
    int y = 370;
    int maxY = 880;
    int lineWidth = 36;
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
