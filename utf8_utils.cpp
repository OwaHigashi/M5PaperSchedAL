#include "globals.h"

bool isUtf8LeadByte(uint8_t c) {
    return (c & 0xC0) != 0x80;
}

int utf8CharBytes(uint8_t c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

String utf8Substring(const String& s, int maxWidth) {
    String result;
    int width = 0;
    int i = 0;

    while (i < (int)s.length() && width < maxWidth) {
        uint8_t c = s[i];
        int bytes = utf8CharBytes(c);

        if (bytes == 4) { i += bytes; continue; }

        int charWidth = (bytes == 1) ? 1 : 2;
        if (width + charWidth > maxWidth) break;

        for (int j = 0; j < bytes && (i + j) < (int)s.length(); j++) {
            result += s[i + j];
        }
        width += charWidth;
        i += bytes;
    }
    return result;
}

// 全角ASCII文字（U+FF01〜U+FF5E）を半角（U+0021〜U+007E）に変換
String normalizeFullWidth(const String& s) {
    String result;
    result.reserve(s.length());
    int i = 0;
    while (i < (int)s.length()) {
        uint8_t b0 = s[i];
        if (b0 == 0xEF && i + 2 < (int)s.length()) {
            uint8_t b1 = s[i + 1];
            uint8_t b2 = s[i + 2];
            if (b1 == 0xBC && b2 >= 0x81 && b2 <= 0xBF) {
                result += (char)(b2 - 0x60);
                i += 3; continue;
            }
            if (b1 == 0xBD && b2 >= 0x80 && b2 <= 0x9E) {
                result += (char)(b2 - 0x20);
                i += 3; continue;
            }
        }
        result += (char)b0;
        i++;
    }
    return result;
}

// HTML簡易デコード
// - ブロック要素(<br>, <p>, <div>, <li>, <h1>-<h6>, <tr>) → "\n"リテラル改行
// - 他のタグ(<b>, <span>, <a>...) → 除去（中身の文字列は残す）
// - 主要エンティティ(&nbsp; &amp; &lt; &gt; &quot; &apos; &#NNN; &#xHH;) → 対応
// - 未対応のエンティティは除去（表示を汚さないため）
String simplifyHtml(const String& s) {
    String result;
    result.reserve(s.length());
    int i = 0;
    int n = (int)s.length();
    while (i < n) {
        char c = s[i];
        if (c == '<') {
            int end = s.indexOf('>', i + 1);
            if (end < 0) { result += s.substring(i); break; }
            String tag = s.substring(i + 1, end);
            tag.trim();
            bool isClosing = false;
            if (tag.length() > 0 && tag[0] == '/') {
                isClosing = true;
                tag = tag.substring(1);
                tag.trim();
            }
            String name;
            for (int k = 0; k < (int)tag.length(); k++) {
                char ch = tag[k];
                if (ch == ' ' || ch == '/' || ch == '\t') break;
                if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
                name += ch;
            }
            bool isBlock = (name == "br" || name == "p" || name == "div" ||
                            name == "li" || name == "tr" ||
                            name == "h1" || name == "h2" || name == "h3" ||
                            name == "h4" || name == "h5" || name == "h6");
            if (isBlock && (name == "br" || name == "p" || name == "div" || isClosing)) {
                result += "\\n";
            }
            i = end + 1;
            continue;
        }
        if (c == '&') {
            int end = s.indexOf(';', i + 1);
            if (end > 0 && end - i <= 10) {
                String ent = s.substring(i + 1, end);
                String entLower = ent;
                entLower.toLowerCase();
                bool handled = true;
                if      (entLower == "nbsp") result += ' ';
                else if (entLower == "amp")  result += '&';
                else if (entLower == "lt")   result += '<';
                else if (entLower == "gt")   result += '>';
                else if (entLower == "quot") result += '"';
                else if (entLower == "apos") result += '\'';
                else if (ent.length() > 1 && ent[0] == '#') {
                    int code = 0;
                    bool valid = true;
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        if (ent.length() < 3) valid = false;
                        for (int k = 2; k < (int)ent.length() && valid; k++) {
                            char ch = ent[k]; int d = -1;
                            if (ch >= '0' && ch <= '9') d = ch - '0';
                            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                            else valid = false;
                            if (valid) code = code * 16 + d;
                        }
                    } else {
                        for (int k = 1; k < (int)ent.length() && valid; k++) {
                            char ch = ent[k];
                            if (ch < '0' || ch > '9') { valid = false; break; }
                            code = code * 10 + (ch - '0');
                        }
                    }
                    if (valid && code >= 0x20 && code < 0x7F) result += (char)code;
                    // 非ASCII/未対応はスキップ
                }
                else handled = false;
                if (handled) { i = end + 1; continue; }
                // 未知のエンティティはまるごと除去
                i = end + 1;
                continue;
            }
        }
        result += c;
        i++;
    }
    return result;
}

// 4バイト文字（絵文字）は '?' に置換、制御文字（0x00-0x1F、ただしタブ以外）は除去
// ※削除ではなく置換にする理由: 絵文字のみのタイトルが空文字になり、[終日]予定が
//   無地表示になる不具合を防ぐため。連続する絵文字は1つの '?' にまとめる。
String removeUnsupportedChars(const String& s) {
    String result;
    bool lastWasEmojiPlaceholder = false;
    int i = 0;
    while (i < (int)s.length()) {
        uint8_t c = s[i];
        int bytes = utf8CharBytes(c);
        if (bytes == 4) {
            if (!lastWasEmojiPlaceholder) {
                result += '?';
                lastWasEmojiPlaceholder = true;
            }
            i += bytes;
            continue;
        }
        if (bytes == 1 && c < 0x20 && c != '\t') {       // 制御文字スキップ
            i++;
            continue;
        }
        for (int j = 0; j < bytes && (i + j) < (int)s.length(); j++) {
            result += s[i + j];
        }
        i += bytes;
        lastWasEmojiPlaceholder = false;
    }
    return result;
}
