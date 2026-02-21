#include "globals.h"
#include "ui_colors.h"
#include <time.h>

//==============================================================================
// DESCRIPTION 内の "\n" リテラル（バックスラッシュ+n の2文字）の位置を返す
// ICS仕様: DESCRIPTION の改行は "\n" という2文字シーケンスで表現される
// なければ -1 を返す
//==============================================================================
static int findLiteralNewline(const String& s) {
    for (int i = 0; i < (int)s.length() - 1; i++) {
        if (s[i] == '\\' && s[i + 1] == 'n') return i;
    }
    return -1;
}

//==============================================================================
// 改行対応テキスト描画ループ
// "\n" リテラルで強制改行、lineWidth で折り返し、スクロール対応
// skipLinesIn: スクロールでスキップする行数（参照渡しで更新）
// yInOut:      現在のY座標（参照渡しで更新）
// 戻り値: 描画しきれなかった残テキストがあれば true（↓スクロール矢印表示用）
//==============================================================================
static bool drawWrappedText(String text, int lineWidth, int lineHeight,
                            int x, int maxY, int& skipLinesIn, int& yInOut) {
    while (text.length() > 0) {
        int nlPos = findLiteralNewline(text);
        String line;
        bool forcedNewline = false;

        if (nlPos == 0) {
            // "\n" が先頭 → 空行
            line = "";
            text = text.substring(2);
            forcedNewline = true;
        } else if (nlPos > 0) {
            // "\n" より前の部分を行幅でクリップ
            String beforeNl = text.substring(0, nlPos);
            line = utf8Substring(beforeNl, lineWidth);
            if (line.length() >= beforeNl.length()) {
                // "\n" まで全部収まった → "\n"(2文字)をスキップ
                text = text.substring(nlPos + 2);
                forcedNewline = true;
            } else {
                // 行幅で折り返した（"\n"はまだ先）
                text = text.substring(line.length());
            }
        } else {
            // "\n" なし → 行幅で折り返し
            line = utf8Substring(text, lineWidth);
            if (line.length() == 0) break;
            text = text.substring(line.length());
        }

        if (skipLinesIn > 0) {
            skipLinesIn--;
            continue;
        }
        if (yInOut >= maxY) {
            return true;  // 描画しきれない行が残っている
        }
        drawText(line, x, yInOut);
        yInOut += lineHeight;
    }
    return false;  // 全行描画完了
}

//==============================================================================
// 詳細画面描画
// フォントサイズは全て 26px 以上（一覧画面の基本フォント=26px と同等以上）
//==============================================================================
void drawDetail(int idx, bool fast) {
    if (idx < 0 || idx >= event_count) return;

    canvas.fillCanvas(COL_DETAIL_BG);
    canvas.setTextColor(COL_DETAIL_TEXT);
    canvas.setTextDatum(TL_DATUM);

    EventItem& e = events[idx];
    struct tm st;
    localtime_r(&e.start, &st);

    // ── 日時ヘッダー (size 30) ──────────────────────────────────────────
    canvas.setTextSize(30);
    char buf[128];
    if (e.is_allday) {
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d [終日]",
                 st.tm_year + 1900, st.tm_mon + 1, st.tm_mday);
    } else {
        String timeStr = formatTime(st.tm_hour, st.tm_min);
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d %s",
                 st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, timeStr.c_str());
    }
    drawText(buf, 10, 8);

    // ── アラーム情報 (size 28) ──────────────────────────────────────────
    canvas.setTextSize(28);
    int y = 48;
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
        drawText(buf, 10, y);
        y += 36;

        // MIDI/再生情報 (size 26 — 最小サイズ)
        canvas.setTextSize(26);
        String extraInfo = "";
        if (strlen(e.midi_file) > 0) {
            extraInfo += e.midi_is_url ? "♪URL:" : "♪SD:";
            extraInfo += e.midi_file;
        }
        int dur = e.play_duration_sec >= 0 ? e.play_duration_sec : config.play_duration;
        if (extraInfo.length() > 0) extraInfo += "  ";
        extraInfo += dur == 0 ? "1曲" : String(dur) + "秒";
        int rep = e.play_repeat >= 0 ? e.play_repeat : config.play_repeat;
        extraInfo += " x" + String(rep);
        drawText(extraInfo, 10, y);
        y += 34;
    } else {
        drawText("アラーム: なし", 10, y);
        y += 36;
    }

    canvas.drawLine(0, y, 540, y, 8);
    y += 8;

    // ── SUMMARY（複数行折り返し、size 30）────────────────────────────────
    canvas.setTextSize(30);
    int summaryLineH = 38;
    int summaryLineW = 32;  // 全角16文字相当（30px × 16 ≈ 480px）
    String summary = removeUnsupportedChars(e.summary());
    while (summary.length() > 0 && y < 300) {
        // SUMMARY に "\n" が含まれる場合も改行として扱う
        int nlPos = findLiteralNewline(summary);
        String line;
        if (nlPos == 0) {
            summary = summary.substring(2);
            y += summaryLineH;
            continue;
        } else if (nlPos > 0) {
            String beforeNl = summary.substring(0, nlPos);
            line = utf8Substring(beforeNl, summaryLineW);
            if (line.length() >= beforeNl.length()) {
                summary = summary.substring(nlPos + 2);
            } else {
                summary = summary.substring(line.length());
            }
        } else {
            line = utf8Substring(summary, summaryLineW);
            if (line.length() == 0) break;
            summary = summary.substring(line.length());
        }
        drawText(line, 10, y);
        y += summaryLineH;
    }

    canvas.drawLine(0, y, 540, y, 6);
    y += 8;

    // ── DESCRIPTION（折り返し＋\nリテラル改行＋スクロール、size 28）──────
    canvas.setTextSize(28);
    const int DESC_LINE_H = 36;
    const int DESC_LINE_W = 32;   // 全角16文字（28px×16≈448px、余白込み）
    const int maxY = 890;
    int skipLines = detail_scroll;

    String desc = removeUnsupportedChars(e.description());
    bool hasMore = drawWrappedText(desc, DESC_LINE_W, DESC_LINE_H,
                                   10, maxY, skipLines, y);

    // ── フッター＆スクロール矢印 ────────────────────────────────────────
    canvas.setTextSize(26);
    drawText("タップ:戻る  L/R:スクロール", 10, 910);
    if (detail_scroll > 0) drawText("↑", 510, 180);
    if (hasMore)            drawText("↓", 510, 850);

    unsigned long t0 = millis();
    canvas.pushCanvas(0, 0, fast ? UPDATE_MODE_DU4 : UPDATE_MODE_GC16);
    Serial.printf("[DETAIL] pushCanvas took %lu ms\n", millis() - t0);
}

//==============================================================================
// アラーム再生中画面
//==============================================================================
void drawPlaying(int idx) {
    if (idx < 0 || idx >= event_count) return;

    canvas.fillCanvas(COL_DETAIL_BG);
    canvas.setTextColor(COL_DETAIL_TEXT);
    canvas.setTextDatum(MC_DATUM);

    EventItem& e = events[idx];

    canvas.setTextSize(52);
    canvas.drawString("ALARM!", 270, 60);

    canvas.setTextSize(34);
    String summary = removeUnsupportedChars(e.summary());
    String line1 = utf8Substring(summary, 28);
    canvas.drawString(line1, 270, 140);
    if (summary.length() > line1.length()) {
        String rest = summary.substring(line1.length());
        String line2 = utf8Substring(rest, 28);
        canvas.drawString(line2, 270, 182);
    }

    struct tm st;
    localtime_r(&e.start, &st);
    String timeStr = formatTime(st.tm_hour, st.tm_min);
    canvas.setTextSize(60);
    canvas.drawString(timeStr, 270, 260);

    canvas.setTextSize(28);
    String info = "";
    if (play_duration_ms > 0) info += String(play_duration_ms / 1000) + "秒";
    else info += "1曲";
    info += " x" + String(play_repeat_remaining) + "回";
    canvas.drawString(info, 270, 330);

    // DESCRIPTION（\nリテラル改行対応）
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(26);
    String desc = removeUnsupportedChars(e.description());
    int y = 380;
    const int maxY = 880;
    int skipLines = 0;
    drawWrappedText(desc, 34, 34, 20, maxY, skipLines, y);

    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(30);
    canvas.drawString("タップで停止", 270, 925);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}
