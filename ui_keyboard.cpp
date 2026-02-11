#include "globals.h"

//==============================================================================
// ファイルスコープ変数（他ファイルから参照不要）
//==============================================================================
static bool keyboard_caps = false;
static bool keyboard_symbol_mode = false;

static const char* kb_rows[] = {
    "1234567890-=",
    "qwertyuiop[]",
    "asdfghjkl;'",
    "zxcvbnm,./\\",
    " "
};
static const int kb_row_count = 5;

static const char* kb_symbol_rows[] = {
    "!@#$%^&*()_+",
    "{}|:\"<>?~`",
    "[]\\;',./",
    " "
};
static const int kb_symbol_row_count = 4;

//==============================================================================
// キーボード描画
//==============================================================================
void drawKeyboard() {
    canvas.fillCanvas(0);
    canvas.setTextColor(15);
    canvas.setTextDatum(TL_DATUM);

    // 編集対象フィールド名
    canvas.setTextSize(26);
    static const char* field_names[] = {
        "WiFi SSID", "WiFi Pass", "ICS URL", "ICS User", "ICS Pass", "", "MIDI URL"
    };
    if (keyboard_target >= 0 && keyboard_target <= SET_MIDI_URL) {
        drawText(String("編集: ") + field_names[keyboard_target], 10, 10);
    }

    canvas.setTextSize(20);
    char lenBuf[16];
    snprintf(lenBuf, sizeof(lenBuf), "%d文字", (int)keyboard_buffer.length());
    drawText(lenBuf, 450, 14);

    canvas.drawLine(0, 48, 540, 48, 12);

    // 入力値表示
    canvas.setTextSize(26);
    String display = keyboard_buffer;
    int y = 62;
    int charsPerLine = 26;
    int maxLines = 12;
    int lineCount = 0;
    while (display.length() > 0 && lineCount < maxLines) {
        int len = min((int)display.length(), charsPerLine);
        drawText(display.substring(0, len), 15, y);
        display = display.substring(len);
        y += 30;
        lineCount++;
    }
    if (lineCount == 0) y = 62;
    drawText("_", 15, y);

    // キーボード部
    canvas.drawLine(0, 455, 540, 455, 12);
    canvas.setTextSize(28);
    int startY = 470;
    int keyW = 42, keyH = 60, spacing = 3;

    const char** rows = keyboard_symbol_mode ? kb_symbol_rows : kb_rows;
    int rowCount = keyboard_symbol_mode ? kb_symbol_row_count : kb_row_count;

    for (int r = 0; r < rowCount; r++) {
        int rowLen = strlen(rows[r]);
        int startX = (540 - rowLen * (keyW + spacing)) / 2;
        int ky = startY + r * (keyH + spacing);

        for (int c = 0; c < rowLen; c++) {
            int x = startX + c * (keyW + spacing);
            canvas.drawRect(x, ky, keyW, keyH, 15);
            char ch = rows[r][c];
            if (!keyboard_symbol_mode && keyboard_caps && ch >= 'a' && ch <= 'z')
                ch = ch - 'a' + 'A';
            char str[2] = {ch, 0};
            if (ch == ' ') canvas.drawString("SP", x + 6, ky + 18);
            else canvas.drawString(str, x + 12, ky + 18);
        }
    }

    // 機能ボタン
    int btnY = startY + rowCount * (keyH + spacing) + 12;
    int btnH = 52;
    canvas.setTextSize(22);
    int bw = 74, bx = 3, bgap = 4;

    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("BS", bx + 22, btnY + 14);
    bx += bw + bgap;

    canvas.drawRect(bx, btnY, bw, btnH, 15);
    if (keyboard_caps) canvas.fillRect(bx+2, btnY+2, bw-4, btnH-4, 8);
    canvas.drawString("CAP", bx + 14, btnY + 14);
    bx += bw + bgap;

    canvas.drawRect(bx, btnY, bw, btnH, 15);
    if (keyboard_symbol_mode) canvas.fillRect(bx+2, btnY+2, bw-4, btnH-4, 8);
    canvas.drawString(keyboard_symbol_mode ? "ABC" : "!@#", bx + 14, btnY + 14);
    bx += bw + bgap;

    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("CLR", bx + 14, btnY + 14);
    bx += bw + bgap;

    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("SP", bx + 22, btnY + 14);
    bx += bw + bgap;

    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("OK", bx + 22, btnY + 14);
    bx += bw + bgap;

    canvas.drawRect(bx, btnY, bw, btnH, 15);
    canvas.drawString("ESC", bx + 16, btnY + 14);

    canvas.setTextSize(20);
    drawText("タップで入力 / OK:保存 / ESC:キャンセル", 10, 925);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

//==============================================================================
// キーボードタップ判定
//==============================================================================
int getKeyboardHit(int tx, int ty) {
    int startY = 470;
    int keyW = 42, keyH = 60, spacing = 3;

    const char** rows = keyboard_symbol_mode ? kb_symbol_rows : kb_rows;
    int rowCount = keyboard_symbol_mode ? kb_symbol_row_count : kb_row_count;

    for (int r = 0; r < rowCount; r++) {
        int rowLen = strlen(rows[r]);
        int startX = (540 - rowLen * (keyW + spacing)) / 2;
        int y = startY + r * (keyH + spacing);
        if (ty >= y && ty < y + keyH) {
            for (int c = 0; c < rowLen; c++) {
                int x = startX + c * (keyW + spacing);
                if (tx >= x && tx < x + keyW) return (r << 8) | c;
            }
        }
    }

    // 機能ボタン
    int btnY = startY + rowCount * (keyH + spacing) + 12;
    int btnH = 52, bw = 74, bgap = 4;
    if (ty >= btnY && ty < btnY + btnH) {
        int bx = 3;
        if (tx >= bx && tx < bx + bw) return -2;  bx += bw + bgap; // BS
        if (tx >= bx && tx < bx + bw) return -1;  bx += bw + bgap; // CAPS
        if (tx >= bx && tx < bx + bw) return -6;  bx += bw + bgap; // 123/ABC
        if (tx >= bx && tx < bx + bw) return -3;  bx += bw + bgap; // CLR
        if (tx >= bx && tx < bx + bw) return -7;  bx += bw + bgap; // SP
        if (tx >= bx && tx < bx + bw) return -4;  bx += bw + bgap; // OK
        if (tx >= bx && tx < bx + bw) return -5;                    // ESC
    }
    return -100;
}

//==============================================================================
// キー入力処理
//==============================================================================
void processKeyboardHit(int hit) {
    if (hit == -100) return;

    if (hit == -1) {
        keyboard_caps = !keyboard_caps;
    } else if (hit == -6) {
        keyboard_symbol_mode = !keyboard_symbol_mode;
    } else if (hit == -2) {
        if (keyboard_buffer.length() > 0) keyboard_buffer.remove(keyboard_buffer.length() - 1);
    } else if (hit == -3) {
        keyboard_buffer = "";
    } else if (hit == -4) {
        // OK — 保存して戻る
        switch (keyboard_target) {
            case SET_WIFI_SSID: strlcpy(config.wifi_ssid, keyboard_buffer.c_str(), sizeof(config.wifi_ssid)); break;
            case SET_WIFI_PASS: strlcpy(config.wifi_pass, keyboard_buffer.c_str(), sizeof(config.wifi_pass)); break;
            case SET_ICS_URL:   strlcpy(config.ics_url, keyboard_buffer.c_str(), sizeof(config.ics_url)); break;
            case SET_ICS_USER:  strlcpy(config.ics_user, keyboard_buffer.c_str(), sizeof(config.ics_user)); break;
            case SET_ICS_PASS:  strlcpy(config.ics_pass, keyboard_buffer.c_str(), sizeof(config.ics_pass)); break;
            case SET_MIDI_URL:  strlcpy(config.midi_url, keyboard_buffer.c_str(), sizeof(config.midi_url)); break;
            case SET_NTFY_TOPIC: strlcpy(config.ntfy_topic, keyboard_buffer.c_str(), sizeof(config.ntfy_topic)); break;
        }
        keyboard_symbol_mode = false;
        keyboard_caps = false;
        ui_state = UI_SETTINGS;
        drawSettings();
        return;
    } else if (hit == -5) {
        // ESC
        keyboard_symbol_mode = false;
        keyboard_caps = false;
        ui_state = UI_SETTINGS;
        drawSettings();
        return;
    } else if (hit == -7) {
        keyboard_buffer += ' ';
    } else {
        const char** rows = keyboard_symbol_mode ? kb_symbol_rows : kb_rows;
        int rowCount = keyboard_symbol_mode ? kb_symbol_row_count : kb_row_count;
        int r = (hit >> 8) & 0xFF;
        int c = hit & 0xFF;
        if (r < rowCount && c < (int)strlen(rows[r])) {
            char ch = rows[r][c];
            if (!keyboard_symbol_mode && keyboard_caps && ch >= 'a' && ch <= 'z')
                ch = ch - 'a' + 'A';
            keyboard_buffer += ch;
        }
    }
    drawKeyboard();
}
