#include "globals.h"
#include "ui_colors.h"
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


//==============================================================================
// 部分更新: ヘッダー時刻のみ（メインcanvas上で再描画 → 該当領域だけEPDにプッシュ）
//==============================================================================
void partialRefreshHeader() {
    // メインcanvas のヘッダー領域を再描画
    canvas.fillRect(0, 0, 540, 40, COL_BG);
    canvas.setTextSize(32);
    canvas.setTextColor(COL_HEADER_TEXT);
    canvas.setTextDatum(TL_DATUM);

    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[64];
    String timeNow = formatTime(lt.tm_hour, lt.tm_min);
    snprintf(buf, sizeof(buf), "%02d/%02d %s", lt.tm_mon + 1, lt.tm_mday, timeNow.c_str());
    canvas.drawString(buf, 10, 8);
    canvas.drawString(buf, 11, 8);

    // WiFi・SD状態
    canvas.setTextSize(22);
    String status = (WiFi.status() == WL_CONNECTED) ? "WiFi:OK" : "WiFi:NG";
    if (!sd_healthy) status += " SD:NG";
    canvas.drawString(status.c_str(), 400, 10);

    // ハートビート状態も反映
    if (heartbeat_visible) {
        canvas.fillCircle(529, 11, 5, 15);
    }

    // メインcanvasバッファの先頭（=ヘッダー領域）だけをEPDへ部分書き込み
    uint8_t* buf_ptr = (uint8_t*)canvas.frameBuffer(1);
    if (!buf_ptr) return;
    M5.EPD.WritePartGram4bpp(0, 0, 540, 40, buf_ptr);
    M5.EPD.UpdateArea(0, 0, 540, 40, UPDATE_MODE_DU4);
    Serial.printf("PARTIAL: header updated %02d:%02d\n", lt.tm_hour, lt.tm_min);
}

//==============================================================================
// 部分更新: 「次のイベント」アンダーラインの移動
//==============================================================================
void partialRefreshNextLine() {
    time_t now = time(nullptr);

    // 現在の「次のイベント」を計算
    int newNextIdx = -1;
    time_t nextTime = 0x7FFFFFFF;
    for (int i = 0; i < event_count; i++) {
        if (!events[i].is_allday && events[i].start > now && events[i].start < nextTime) {
            nextTime = events[i].start;
            newNextIdx = i;
        }
    }

    if (newNextIdx == displayed_next_event_idx) return;  // 変化なし

    int rowH = 46;
    uint8_t* buf_ptr = (uint8_t*)canvas.frameBuffer(1);
    if (!buf_ptr) return;
    int stride = 540 / 2;  // 270 bytes/row (4bit grayscale)

    // 旧アンダーラインを消去
    if (displayed_next_event_idx >= 0) {
        for (int d = 0; d < displayed_count; d++) {
            if (row_event_idx[d] == displayed_next_event_idx) {
                int y = row_y0[d];
                int bg = (row_event_idx[d] == selected_event) ? COL_CURSOR_BG : COL_BG;
                canvas.fillRect(10, y + rowH - 5, 520, 2, bg);
                // アンダーライン付近の8行分を部分プッシュ
                int strip_y = y + rowH - 8;
                M5.EPD.WritePartGram4bpp(0, strip_y, 540, 8, buf_ptr + strip_y * stride);
                M5.EPD.UpdateArea(0, strip_y, 540, 8, UPDATE_MODE_DU);
                break;
            }
        }
    }

    // 新アンダーラインを描画
    if (newNextIdx >= 0) {
        for (int d = 0; d < displayed_count; d++) {
            if (row_event_idx[d] == newNextIdx) {
                int y = row_y0[d];
                canvas.drawLine(10, y + rowH - 5, 530, y + rowH - 5, COL_NEXT_EVENT_LINE);
                canvas.drawLine(10, y + rowH - 4, 530, y + rowH - 4, COL_NEXT_EVENT_LINE);
                int strip_y = y + rowH - 8;
                M5.EPD.WritePartGram4bpp(0, strip_y, 540, 8, buf_ptr + strip_y * stride);
                M5.EPD.UpdateArea(0, strip_y, 540, 8, UPDATE_MODE_DU);
                break;
            }
        }
    }

    Serial.printf("PARTIAL: next-event line %d -> %d\n", displayed_next_event_idx, newNextIdx);
    displayed_next_event_idx = newNextIdx;
}
