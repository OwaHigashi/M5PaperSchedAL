#include "globals.h"
#include "ui_colors.h"
#include <time.h>

void drawText(const String& s, int x, int y) {
    canvas.drawString(s, x, y);
}

// 太字描画: level 0=通常, 1=(x+1), 2=(x+1,y+1), 3=(x+1,y+1,x+1y+1)
void drawTextBold(const String& s, int x, int y, int level) {
    canvas.drawString(s, x, y);
    if (level >= 1) canvas.drawString(s, x + 1, y);
    if (level >= 2) canvas.drawString(s, x, y + 1);
    if (level >= 3) canvas.drawString(s, x + 1, y + 1);
}

// スクリーンショット: ホストへ送信 (data/screenshots/*.png)
void saveScreenshot() {
    if (!uploadScreenshot()) {
        Serial.println("Screenshot: upload failed");
        logLine("screenshot upload failed");
    }
}

// ヘッダー右側の状態文字列:
//   "HH:MM"  最終同期時刻   " fchNX" ホスト側ICS失敗   " !H" ホスト不達   " !W" WiFi断   " !T" 時刻未設定
void buildStatusText(char* buf, size_t size) {
    int spos = 0;
    if (last_fetch > 1000000000) {
        struct tm ft; localtime_r(&last_fetch, &ft);
        spos += snprintf(buf + spos, size - spos, "%02d:%02d", ft.tm_hour, ft.tm_min);
    } else {
        spos += snprintf(buf + spos, size - spos, "--:--");
    }
    for (int i = 0; i < 8 && spos < (int)size - 8; i++) {
        if (src_fail_mask & (1 << i)) spos += snprintf(buf + spos, size - spos, " fch%dX", i + 1);
    }
    if (WiFi.status() != WL_CONNECTED) spos += snprintf(buf + spos, size - spos, " !W");
    else if (!host_online)             spos += snprintf(buf + spos, size - spos, " !H");
    if (!time_valid)                   spos += snprintf(buf + spos, size - spos, " !T");
}

// ホストからのメッセージ表示 (一定時間後に元画面へ)
void showMessage(const char* text, int holdMs) {
    canvas.fillCanvas(0);
    canvas.setTextColor(15);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(32);
    drawTextBold("ホストからのメッセージ", 270, 120, 2);
    canvas.setTextSize(30);
    String t = removeUnsupportedChars(text);
    int y = 220;
    while (t.length() > 0 && y < 850) {
        String line = utf8Substring(t, 32);
        if (line.length() == 0) break;
        drawTextBold(line, 270, y, 2);
        t = t.substring(line.length());
        y += 40;
    }
    canvas.setTextDatum(TL_DATUM);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    unsigned long t0 = millis();
    while (millis() - t0 < (unsigned long)holdMs) { delay(50); }
    if (ui_state == UI_LIST) drawList();
    else if (ui_state == UI_DETAIL) drawDetail(selected_event);
    else if (ui_state == UI_SETTINGS) drawSettings();
}

String formatTime(int hour, int minute) {
    char buf[16];
    if (config.time_24h) {
        snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    } else {
        // 12時間制: AM/PM の見落とし(夜の予定を朝と誤認)を防ぐため A/P を付与
        const char* ap = (hour < 12) ? "A" : "P";
        int h12 = hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, sizeof(buf), "%2d:%02d%s", h12, minute, ap);
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

    canvas.setTextSize(22);
    char statusBuf[96];
    buildStatusText(statusBuf, sizeof(statusBuf));
    canvas.setTextColor(COL_HEADER_TEXT);
    canvas.drawString(statusBuf, 260, 10);

    // ハートビート状態も反映
    if (heartbeat_visible) {
        canvas.fillCircle(529, 11, 5, 15);
    }

    // メインcanvasバッファの先頭（=ヘッダー領域）だけをEPDへ部分書き込み
    uint8_t* buf_ptr = (uint8_t*)canvas.frameBuffer(1);
    if (!buf_ptr) return;
    M5.EPD.WritePartGram4bpp(0, 0, 540, 40, buf_ptr);
    M5.EPD.UpdateArea(0, 0, 540, 40, UPDATE_MODE_GL16);
    Serial.printf("PARTIAL: header updated %02d:%02d\n", lt.tm_hour, lt.tm_min);
}

//==============================================================================
// 「次の予定」マーカー = ▶（太字）
//   音符♪列の隣の専用列(LIST_NEXT_MARK_X)に太字の▶を描く。
//   選択行の下線とは別表現なので一目で区別できる。
//==============================================================================
void drawNextEventMarker(int y, int rowH) {
    (void)rowH;
    canvas.setTextSize(28);
    canvas.setTextColor(COL_ROW_TEXT);
    drawTextBold("▶", LIST_NEXT_MARK_X, y + 9, 2);   // ▶ U+25B6
    canvas.setTextColor(COL_TEXT);
}

//==============================================================================
// 部分更新: 「次のイベント」▶マーカーの移動
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

    // 旧▶マークを消去（▶列の周辺のみクリア。下線/本文は触らない）
    if (displayed_next_event_idx >= 0) {
        for (int d = 0; d < displayed_count; d++) {
            if (row_event_idx[d] == displayed_next_event_idx) {
                int y = row_y0[d];
                canvas.fillRect(LIST_NEXT_MARK_X - 2, y + 4, 32, rowH - 8, COL_BG);
                // 行全体(全幅)の縦ストリップを部分プッシュ（4bppは全幅でストライド一致）
                M5.EPD.WritePartGram4bpp(0, y, 540, rowH, buf_ptr + y * stride);
                M5.EPD.UpdateArea(0, y, 540, rowH, UPDATE_MODE_DU);
                break;
            }
        }
    }

    // 新▶マークを描画
    if (newNextIdx >= 0) {
        for (int d = 0; d < displayed_count; d++) {
            if (row_event_idx[d] == newNextIdx) {
                int y = row_y0[d];
                drawNextEventMarker(y, rowH);   // ▶（太字）
                M5.EPD.WritePartGram4bpp(0, y, 540, rowH, buf_ptr + y * stride);
                M5.EPD.UpdateArea(0, y, 540, rowH, UPDATE_MODE_DU);
                break;
            }
        }
    }

    Serial.printf("PARTIAL: next-event line %d -> %d\n", displayed_next_event_idx, newNextIdx);
    displayed_next_event_idx = newNextIdx;
}
