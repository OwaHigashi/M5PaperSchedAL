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

// 4バイト文字（絵文字）と制御文字（0x00-0x1F、ただしタブ・改行は許可）を除去
String removeUnsupportedChars(const String& s) {
    String result;
    int i = 0;
    while (i < (int)s.length()) {
        uint8_t c = s[i];
        int bytes = utf8CharBytes(c);
        if (bytes == 4) { i += bytes; continue; }       // 絵文字スキップ
        if (bytes == 1 && c < 0x20 && c != '\t') {       // 制御文字スキップ
            i++; continue;
        }
        for (int j = 0; j < bytes && (i + j) < (int)s.length(); j++) {
            result += s[i + j];
        }
        i += bytes;
    }
    return result;
}
