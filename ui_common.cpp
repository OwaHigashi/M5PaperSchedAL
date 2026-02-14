#include "globals.h"
#include <time.h>
#include <SD.h>

void drawText(const String& s, int x, int y) {
    canvas.drawString(s, x, y);
}

// スクリーンショットをPGM形式でSDに保存
void saveScreenshot() {
    waitEPDReady();
    uint32_t bufferSize = canvas.getBufferSize();
    uint8_t *buffer = (uint8_t *)(canvas.frameBuffer(1));
    int width = canvas.width();
    int height = canvas.height();

    // フォルダ作成
    if (!SD.exists("/screenshots")) SD.mkdir("/screenshots");

    // 連番ファイル名
    int idx = 1;
    char fname[48];
    do {
        snprintf(fname, sizeof(fname), "/screenshots/ss%d.pgm", idx++);
    } while (SD.exists(fname) && idx < 999);

    File f = SD.open(fname, FILE_WRITE);
    if (!f) {
        Serial.println("Screenshot: failed to open file");
        return;
    }

    // PGMヘッダ
    f.printf("P5 %d %d 255 ", width, height);

    // 4bitグレースケール→8bitに変換して書き出し
    for (uint32_t i = 0; i < bufferSize; i++) {
        uint8_t byte = buffer[i];
        f.write((uint8_t)(17 * (15 - (byte >> 4))));
        f.write((uint8_t)(17 * (15 - (byte & 0x0F))));
    }
    f.close();
    Serial.printf("Screenshot saved: %s (%dx%d)\n", fname, width, height);
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
