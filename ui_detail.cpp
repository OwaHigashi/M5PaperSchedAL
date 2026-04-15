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
                            int x, int maxY, int& skipLinesIn, int& yInOut,
                            int boldLevel = 0) {
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
            if (line.length() == 0) {
                // 折り返し不能文字 → 1バイト進めて無限ループ防止
                text = text.substring(1);
                continue;
            }
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
        drawTextBold(line, x, yInOut, boldLevel);
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

    // ★ 詳細画面に入るときはパネルを完全リフレッシュ
    //    一覧画面の部分更新による残像で文字が薄く見える問題を解消
    if (!fast) {
        waitEPDReady();
        M5.EPD.Clear(true);
        waitEPDReady();
    }

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
    drawTextBold(buf, 10, 8, 2);

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
        drawTextBold(buf, 10, y, 2);
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
        drawTextBold(extraInfo, 10, y, 2);
        y += 34;
    } else {
        drawTextBold("アラーム: なし", 10, y, 2);
        y += 36;
    }

    canvas.drawLine(0, y, 540, y, 12);
    canvas.drawLine(0, y + 1, 540, y + 1, 12);
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
            if (line.length() == 0) {
                summary = summary.substring(1);
                continue;
            }
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
        drawTextBold(line, 10, y, 2);
        y += summaryLineH;
    }

    canvas.drawLine(0, y, 540, y, 10);
    canvas.drawLine(0, y + 1, 540, y + 1, 10);
    y += 8;

    // ── DESCRIPTION（折り返し＋\nリテラル改行＋スクロール、size 28）──────
    canvas.setTextSize(28);
    const int DESC_LINE_H = 36;
    const int DESC_LINE_W = 32;   // 全角16文字（28px×16≈448px、余白込み）
    const int maxY = 890;
    int skipLines = detail_scroll;

    String desc = removeUnsupportedChars(e.description());
    bool hasMore = drawWrappedText(desc, DESC_LINE_W, DESC_LINE_H,
                                   10, maxY, skipLines, y, 3);

    // ── フッター＆スクロール矢印 ────────────────────────────────────────
    canvas.setTextSize(26);
    drawTextBold("タップ:戻る  L/R:スクロール", 10, 910, 2);
    if (detail_scroll > 0) drawTextBold("↑", 510, 180, 2);
    if (hasMore)            drawTextBold("↓", 510, 850, 2);

    unsigned long t0 = millis();
    // スクロール時もGC16を使用（DU4はコントラストが低い）
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
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
    drawTextBold("ALARM!", 270, 60, 3);

    canvas.setTextSize(34);
    String summary = removeUnsupportedChars(e.summary());
    String line1 = utf8Substring(summary, 28);
    drawTextBold(line1, 270, 140, 2);
    if (summary.length() > line1.length()) {
        String rest = summary.substring(line1.length());
        String line2 = utf8Substring(rest, 28);
        drawTextBold(line2, 270, 182, 2);
    }

    struct tm st;
    localtime_r(&e.start, &st);
    String timeStr = formatTime(st.tm_hour, st.tm_min);
    canvas.setTextSize(60);
    drawTextBold(timeStr, 270, 260, 3);

    canvas.setTextSize(28);
    String info = "";
    if (play_duration_ms > 0) info += String(play_duration_ms / 1000) + "秒";
    else info += "1曲";
    info += " x" + String(play_repeat_remaining) + "回";
    drawTextBold(info, 270, 330, 2);

    // DESCRIPTION（\nリテラル改行対応）
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(26);
    String desc = removeUnsupportedChars(e.description());
    int y = 380;
    const int maxY = 880;
    int skipLines = 0;
    drawWrappedText(desc, 34, 34, 20, maxY, skipLines, y, 3);

    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(30);
    drawTextBold("タップで停止", 270, 925, 2);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}
