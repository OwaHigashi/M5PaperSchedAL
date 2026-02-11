#include "globals.h"
#include <time.h>

void drawText(const String& s, int x, int y) {
    canvas.drawString(s, x, y);
}

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

void drawHeader(const char* title) {
    canvas.setTextSize(24);
    canvas.setTextColor(15);
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);

    char buf[64];
    snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d  %s",
             lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, title);
    drawText(buf, 10, 10);
}
