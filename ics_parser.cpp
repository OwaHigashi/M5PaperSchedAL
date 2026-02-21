#include "globals.h"
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <ctype.h>

// ★ v029: ics_parser内のString完全排除 — char[]固定バッファのみ使用
//    DRAM断片化の最大原因だった動的String確保/解放を根絶

//==============================================================================
// パーサー用バッファサイズ定数
//==============================================================================
static const int LINE_BUF      = 4096;  // ICS 1行 (unfold後)
static const int PUSHBACK_BUF  = 4096;  // unfold pushback用
static const int DTSTART_BUF   = 32;    // "20250214T120000Z" 程度
static const int SUMMARY_BUF   = 512;   // タイトル
static const int DESC_BUF      = 2048;  // 説明文
static const int MIDI_FILE_BUF = 128;   // MIDIファイル名
static const int CONTENT_BUF   = 256;   // アラームマーカー内容
static const int NORM_BUF      = 512;   // 全角正規化用

//==============================================================================
// char ユーティリティ
//==============================================================================

// 先頭・末尾の空白を除去（in-place）
static void trimBuf(char* s) {
    int start = 0;
    while (s[start] && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) start++;
    if (start > 0) {
        int i = 0;
        while (s[start + i]) { s[i] = s[start + i]; i++; }
        s[i] = '\0';
    }
    int len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
}

// 全角ASCII→半角変換 (char版 normalizeFullWidth)
static void normalizeFullWidthBuf(const char* src, char* dst, int dstSize) {
    int di = 0;
    int i = 0;
    int srcLen = strlen(src);
    while (i < srcLen && di < dstSize - 1) {
        uint8_t b0 = (uint8_t)src[i];
        if (b0 == 0xEF && i + 2 < srcLen) {
            uint8_t b1 = (uint8_t)src[i + 1];
            uint8_t b2 = (uint8_t)src[i + 2];
            if (b1 == 0xBC && b2 >= 0x81 && b2 <= 0xBF) {
                dst[di++] = (char)(b2 - 0x60);
                i += 3; continue;
            }
            if (b1 == 0xBD && b2 >= 0x80 && b2 <= 0x9E) {
                dst[di++] = (char)(b2 - 0x20);
                i += 3; continue;
            }
        }
        dst[di++] = src[i++];
    }
    dst[di] = '\0';
}

// strncpyの安全版（常にNUL終端）
static void safeCopy(char* dst, const char* src, int dstSize) {
    strlcpy(dst, src, dstSize);
}

// 部分文字列コピー（src[from..to-1]をdstへ）
static void substrCopy(char* dst, const char* src, int from, int to, int dstSize) {
    int srcLen = strlen(src);
    if (from < 0) from = 0;
    if (to > srcLen) to = srcLen;
    int copyLen = to - from;
    if (copyLen <= 0) { dst[0] = '\0'; return; }
    if (copyLen >= dstSize) copyLen = dstSize - 1;
    memcpy(dst, src + from, copyLen);
    dst[copyLen] = '\0';
}

// atoi相当の安全版（空白トリム込み）
static int safeAtoi(const char* s) {
    while (*s == ' ') s++;
    return atoi(s);
}

//==============================================================================
// 日時パース
//==============================================================================
bool parseDT(const char* raw, time_t& out, bool& is_allday) {
    char s[DTSTART_BUF];
    safeCopy(s, raw, DTSTART_BUF);
    trimBuf(s);

    int slen = strlen(s);
    bool utc = (slen > 0 && s[slen - 1] == 'Z');
    if (utc) s[slen - 1] = '\0';
    slen = strlen(s);

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    is_allday = false;

    // 数字部分を直接抽出（substring不要）
    auto dig2 = [](const char* p) -> int { return (p[0] - '0') * 10 + (p[1] - '0'); };
    auto dig4 = [](const char* p) -> int { return (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0'); };

    if (slen == 8) {
        y  = dig4(s);
        mo = dig2(s + 4);
        d  = dig2(s + 6);
        is_allday = true;
    } else if (slen >= 15 && s[8] == 'T') {
        y  = dig4(s);
        mo = dig2(s + 4);
        d  = dig2(s + 6);
        h  = dig2(s + 9);
        mi = dig2(s + 11);
        se = dig2(s + 13);
    } else {
        return false;
    }

    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = se;

    if (!utc) {
        out = mktime(&t);
        return out != (time_t)-1;
    }

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t u = mktime(&t);
    setenv("TZ", TZ_JST, 1);
    tzset();
    out = u;
    return out != (time_t)-1;
}

//==============================================================================
// アラームマーカーパーサー
//==============================================================================
bool parseAlarmMarker(const char* s_raw, bool is_summary, int& off, bool& found,
                      char* midi_file, int midi_file_size, bool& midi_is_url,
                      int& duration_sec, int& repeat_count) {
    static char* norm = nullptr;  // PSRAM上に配置
    if (!norm) norm = (char*)ps_malloc(NORM_BUF);
    normalizeFullWidthBuf(s_raw, norm, NORM_BUF);
    const char* s = norm;
    int sLen = strlen(s);

    found = false;
    off = config.alarm_offset_default;
    midi_file[0] = '\0';
    midi_is_url = false;
    duration_sec = -1;
    repeat_count = -1;
    int maxOff = -1;

    if (is_summary) {
        int exclamCount = 0;
        for (int i = 0; i < sLen; i++) {
            if (s[i] == '!') exclamCount++;
        }
        if (exclamCount == 1) {
            found = true;
            return true;
        }
    }

    int searchStart = 0;
    while (searchStart < sLen) {
        const char* pPtr = strchr(s + searchStart, '!');
        if (!pPtr) break;
        int p = pPtr - s;

        const char* ePtr = strchr(s + p + 1, '!');
        if (!ePtr) {
            if (is_summary) { found = true; return true; }
            break;
        }
        int endExcl = ePtr - s;

        found = true;

        static char* content = nullptr;
        if (!content) content = (char*)ps_malloc(CONTENT_BUF);
        substrCopy(content, s, p + 1, endExcl, CONTENT_BUF);

        int thisOff = config.alarm_offset_default;
        bool thisIsUrl = false;
        char thisFile[MIDI_FILE_BUF];
        thisFile[0] = '\0';

        int cLen = strlen(content);
        int i = 0;
        while (i < cLen) {
            char c = content[i];
            if (c == '-' || c == '+') {
                int numStart = i + 1, numEnd = numStart;
                while (numEnd < cLen && (isdigit(content[numEnd]) || content[numEnd] == ' ')) numEnd++;
                if (numEnd > numStart) {
                    char numBuf[16];
                    substrCopy(numBuf, content, numStart, numEnd, sizeof(numBuf));
                    int val = safeAtoi(numBuf);
                    if (val >= 0 && val <= 24 * 60) thisOff = (c == '-') ? val : -val;
                }
                i = numEnd;
            } else if (c == '>' || c == '<') {
                thisIsUrl = (c == '>');
                int fileStart = i + 1, fileEnd = fileStart;
                while (fileEnd < cLen &&
                       content[fileEnd] != '-' && content[fileEnd] != '+' &&
                       content[fileEnd] != '@' && content[fileEnd] != '*' &&
                       content[fileEnd] != '>' && content[fileEnd] != '<') fileEnd++;
                if (fileEnd > fileStart) {
                    substrCopy(thisFile, content, fileStart, fileEnd, MIDI_FILE_BUF);
                    trimBuf(thisFile);
                }
                i = fileEnd;
            } else if (c == '@') {
                int numStart = i + 1, numEnd = numStart;
                while (numEnd < cLen && (isdigit(content[numEnd]) || content[numEnd] == ' ')) numEnd++;
                if (numEnd > numStart) {
                    char numBuf[16];
                    substrCopy(numBuf, content, numStart, numEnd, sizeof(numBuf));
                    duration_sec = safeAtoi(numBuf);
                } else {
                    duration_sec = 0;
                }
                i = numEnd;
            } else if (c == '*') {
                int numStart = i + 1, numEnd = numStart;
                while (numEnd < cLen && (isdigit(content[numEnd]) || content[numEnd] == ' ')) numEnd++;
                if (numEnd > numStart) {
                    char numBuf[16];
                    substrCopy(numBuf, content, numStart, numEnd, sizeof(numBuf));
                    repeat_count = safeAtoi(numBuf);
                }
                i = numEnd;
            } else {
                i++;
            }
        }

        if (abs(thisOff) > abs(maxOff) || maxOff == -1) maxOff = thisOff;
        if (thisFile[0] != '\0') {
            safeCopy(midi_file, thisFile, midi_file_size);
            midi_is_url = thisIsUrl;
        }
        searchStart = endExcl + 1;
    }

    if (found && maxOff != -1) off = maxOff;
    return found;
}

//==============================================================================
// ソート・切り詰め
//==============================================================================
void sortEvents() {
    for (int i = 0; i < event_count; i++) {
        for (int j = i + 1; j < event_count; j++) {
            if (events[j].start < events[i].start) {
                EventItem tmp = events[i];
                events[i] = events[j];
                events[j] = tmp;
            }
        }
    }
}

void trimEventsAroundToday(int maxEvents) {
    if (event_count <= maxEvents) return;

    time_t now = time(nullptr);
    int today_idx = event_count;
    for (int i = 0; i < event_count; i++) {
        if (events[i].start >= now - 86400) { today_idx = i; break; }
    }

    int future_count = event_count - today_idx;
    int keep_past = min(today_idx, min(10, maxEvents - future_count));
    if (keep_past < 0) keep_past = 0;
    int start_idx = today_idx - keep_past;
    if (start_idx < 0) start_idx = 0;

    Serial.printf("TRIM: total=%d, today=%d, future=%d, keep_past=%d, start=%d\n",
                  event_count, today_idx, future_count, keep_past, start_idx);

    if (start_idx > 0) {
        for (int i = 0; i < event_count - start_idx; i++) {
            events[i] = events[i + start_idx];
        }
        event_count -= start_idx;
    }

    if (event_count > maxEvents) {
        event_count = maxEvents;
    }
    Serial.printf("TRIM: result=%d events\n", event_count);
}

//==============================================================================
// ストリーミングICSパーサー（String完全排除版）
//==============================================================================

// ストリームから1行読み取り（CRLF/LF区切り）→ char buf版
static bool readRawLine(WiFiClient* stream, char* buf, int bufSize) {
    int len = 0;
    buf[0] = '\0';
    unsigned long timeout = millis() + 10000;

    while (stream->connected() || stream->available()) {
        if (millis() > timeout) {
            Serial.println("ICS_STREAM: readline timeout");
            buf[len] = '\0';
            return len > 0;
        }
        if (!stream->available()) { delay(1); continue; }

        char c = stream->read();
        if (c == '\r') continue;
        if (c == '\n') { buf[len] = '\0'; return true; }
        if (len < bufSize - 1) buf[len++] = c;
    }
    buf[len] = '\0';
    return len > 0;
}

// RFC 5545 unfold処理統合 — 次行が空白/タブで始まる場合は結合
static bool readUnfoldedLine(WiFiClient* stream, char* line, int lineSize,
                             char* pushback, int pushbackSize) {
    if (pushback[0] != '\0') {
        safeCopy(line, pushback, lineSize);
        pushback[0] = '\0';
    } else {
        if (!readRawLine(stream, line, lineSize)) return false;
    }

    int lineLen = strlen(line);

    while (stream->connected() || stream->available()) {
        // 次の行をtempバッファに仮読み（PSRAM上に確保）
        static char* nextLine = nullptr;
        if (!nextLine) nextLine = (char*)ps_malloc(LINE_BUF);
        if (!readRawLine(stream, nextLine, LINE_BUF)) break;

        if (nextLine[0] == ' ' || nextLine[0] == '\t') {
            // unfold: 先頭空白を飛ばして結合
            int addLen = strlen(nextLine + 1);
            if (lineLen + addLen < lineSize - 1) {
                memcpy(line + lineLen, nextLine + 1, addLen);
                lineLen += addLen;
                line[lineLen] = '\0';
            }
        } else {
            // 次の論理行 → pushbackに保存
            safeCopy(pushback, nextLine, pushbackSize);
            return true;
        }
    }
    return true;
}

// 1つのVEVENTをevents[]に登録
static void registerEvent(const char* dtstart_raw, const char* summary, const char* desc) {
    if (event_count >= MAX_EVENTS) return;

    time_t now = time(nullptr);
    time_t st = 0;
    bool is_allday = false;
    if (!parseDT(dtstart_raw, st, is_allday)) return;
    if (st <= now - 86400 || st >= now + 30 * 86400) return;

    int off = 0;
    bool hasAL = false;
    char midi_file_str[MIDI_FILE_BUF];
    midi_file_str[0] = '\0';
    bool midi_is_url_flag = false;
    int ev_duration = -1, ev_repeat = -1;

    parseAlarmMarker(summary, true, off, hasAL, midi_file_str, MIDI_FILE_BUF,
                     midi_is_url_flag, ev_duration, ev_repeat);
    if (!hasAL && desc[0] != '\0') {
        parseAlarmMarker(desc, false, off, hasAL, midi_file_str, MIDI_FILE_BUF,
                         midi_is_url_flag, ev_duration, ev_repeat);
    }

    if (hasAL) {
        char logSum[41];
        substrCopy(logSum, summary, 0, 40, sizeof(logSum));
        Serial.printf("ICS_STREAM: [%d] ALARM '%s' offset=%d\n", event_count, logSum, off);
    }

    int idx = event_count;
    events[idx].start = st;

    // text[] に summary \0 description \0 を格納
    int bufSize = sizeof(events[idx].text);
    int sumLen = strlen(summary);
    if (sumLen >= bufSize - 2) sumLen = bufSize - 2;
    memcpy(events[idx].text, summary, sumLen);
    events[idx].text[sumLen] = '\0';

    int descPos = sumLen + 1;
    int descSpace = bufSize - descPos - 1;
    int maxDesc = min(descSpace, config.max_desc_bytes);
    int descLen = strlen(desc);
    if (descLen <= maxDesc) {
        memcpy(events[idx].text + descPos, desc, descLen);
        events[idx].text[descPos + descLen] = '\0';
    } else {
        // UTF-8境界で切り詰め
        int cutAt = maxDesc;
        while (cutAt > 0 && ((uint8_t)desc[cutAt] & 0xC0) == 0x80) cutAt--;
        memcpy(events[idx].text + descPos, desc, cutAt);
        events[idx].text[descPos + cutAt] = '\0';
    }

    events[idx].has_alarm = hasAL;
    events[idx].is_allday = is_allday;
    strlcpy(events[idx].midi_file, midi_file_str, sizeof(events[idx].midi_file));
    events[idx].midi_is_url = midi_is_url_flag;
    events[idx].play_duration_sec = ev_duration;
    events[idx].play_repeat = ev_repeat;

    if (hasAL) {
        events[idx].offset_min = off;
        events[idx].alarm_time = st - (time_t)off * 60;
        const time_t ALARM_GRACE_SEC = 600;
        events[idx].triggered = (events[idx].alarm_time < now - ALARM_GRACE_SEC);
    } else {
        events[idx].offset_min = 0;
        events[idx].alarm_time = 0;
        events[idx].triggered = false;
    }
    event_count++;
}

// ストリーミングパーサー本体
static int parseICSStream(WiFiClient* stream) {
    bool inEvent = false;
    int parsed_events = 0;

    // ★ パーサーバッファをPSRAMに配置 → DRAM .bss を節約
    //    5分毎に呼ばれるが、同時実行はないので static で安全
    static char* line = nullptr;
    static char* pushback = nullptr;
    static char* desc = nullptr;
    if (!line) {
        line     = (char*)ps_malloc(LINE_BUF);
        pushback = (char*)ps_malloc(PUSHBACK_BUF);
        desc     = (char*)ps_malloc(DESC_BUF);
    }
    // スタック節約: summary も static (同時実行なし)
    char dtstart_raw[DTSTART_BUF];
    static char summary[SUMMARY_BUF];

    line[0] = '\0';
    pushback[0] = '\0';
    dtstart_raw[0] = '\0';
    summary[0] = '\0';
    desc[0] = '\0';

    Serial.printf("ICS_STREAM: Start parsing (heap: %d)\n", ESP.getFreeHeap());

    while (readUnfoldedLine(stream, line, LINE_BUF, pushback, PUSHBACK_BUF)) {
        trimBuf(line);
        if (line[0] == '\0') continue;

        if (strcmp(line, "BEGIN:VEVENT") == 0) {
            inEvent = true;
            dtstart_raw[0] = '\0';
            summary[0] = '\0';
            desc[0] = '\0';
            continue;
        }

        if (strcmp(line, "END:VEVENT") == 0) {
            parsed_events++;
            if (inEvent) {
                registerEvent(dtstart_raw, summary, desc);
                if (event_count >= MAX_EVENTS) {
                    Serial.println("ICS_STREAM: MAX_EVENTS reached");
                    break;
                }
            }
            inEvent = false;
            dtstart_raw[0] = '\0';
            summary[0] = '\0';
            desc[0] = '\0';
            continue;
        }

        if (!inEvent) continue;

        // プロパティ解析: "KEY;PARAMS:VALUE" または "KEY:VALUE"
        const char* colon = strchr(line, ':');
        if (!colon) continue;

        if (strncmp(line, "DTSTART", 7) == 0 && (line[7] == ':' || line[7] == ';')) {
            safeCopy(dtstart_raw, colon + 1, DTSTART_BUF);
        } else if (strncmp(line, "SUMMARY", 7) == 0 && (line[7] == ':' || line[7] == ';')) {
            safeCopy(summary, colon + 1, SUMMARY_BUF);
        } else if (strncmp(line, "DESCRIPTION", 11) == 0 && (line[11] == ':' || line[11] == ';')) {
            const char* val = colon + 1;
            int valLen = strlen(val);
            int maxParseLen = 2000;
            if (valLen > maxParseLen) {
                memcpy(desc, val, maxParseLen);
                desc[maxParseLen] = '\0';
            } else {
                safeCopy(desc, val, DESC_BUF);
            }
        }
    }

    Serial.printf("ICS_STREAM: Complete - parsed %d VEVENTs, loaded %d (heap: %d)\n",
                  parsed_events, event_count, ESP.getFreeHeap());

    // ★ sortEvents/trimEventsAroundToday は呼ばない
    //   複数URL対応: 全URL fetch後にfetchAndUpdate()側で実行
    return event_count;
}

//==============================================================================
// ICS取得 + ストリーミング解析
//==============================================================================

// ★ HTTPヘッダーを1行読み取り (DRAM malloc ゼロ)
// 戻り値: 行の長さ (0=空行=ヘッダー終了, -1=切断/タイムアウト)
static int readHeaderLine(WiFiClient* c, char* buf, int maxLen) {
    int i = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 15000) {
        if (!c->connected() && !c->available()) return -1;
        if (!c->available()) { delay(1); continue; }
        int b = c->read();
        if (b < 0) continue;
        if (b == '\n') break;
        if (b != '\r' && i < maxLen - 1) buf[i++] = b;
    }
    buf[i] = '\0';
    return i;
}

// 1つのURLをHTTP取得+パースを実行。成功した件数を返す。-1で失敗。
// ★ バッファ管理は呼び出し側(fetchAndUpdate)が行う
// ★ events[]にアペンド（event_countは増加していく）
static int doFetchURL(const char* url_str) {
    // ── URL解析 (static — スタック節約、同時実行なし) ──
    static char host[128];
    static char path[256];
    int port = 443;
    bool use_ssl = true;

    const char* url = url_str;
    if (strncmp(url, "https://", 8) == 0) { url += 8; }
    else if (strncmp(url, "http://", 7) == 0) { url += 7; port = 80; use_ssl = false; }

    const char* pathStart = strchr(url, '/');
    if (pathStart) {
        int hostLen = pathStart - url;
        if (hostLen >= (int)sizeof(host)) hostLen = sizeof(host) - 1;
        memcpy(host, url, hostLen);
        host[hostLen] = '\0';
        safeCopy(path, pathStart, sizeof(path));
    } else {
        safeCopy(host, url, sizeof(host));
        strcpy(path, "/");
    }

    // ポート番号抽出 (host:port 形式)
    char* colonInHost = strchr(host, ':');
    if (colonInHost) {
        port = atoi(colonInHost + 1);
        *colonInHost = '\0';
    }

    Serial.printf("Raw HTTPS: host=%s port=%d path=%.40s...\n", host, port, path);

    int count_before = event_count;  // このURL fetch前の件数

    // ── SSL接続 ──
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);
    Serial.printf("SSL client initialized (heap: %d, maxBlock: %d)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    if (!client.connect(host, port)) {
        Serial.printf("SSL connect failed (heap:%d maxBlock:%d)\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return -1;
    }
    Serial.printf("SSL connected (heap:%d maxBlock:%d)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // ── HTTPリクエスト構築 (static — スタック節約) ──
    static char authLine[512];
    authLine[0] = '\0';
    if (strlen(config.ics_user) > 0) {
        static char auth_raw[256];
        snprintf(auth_raw, sizeof(auth_raw), "%s:%s", config.ics_user, config.ics_pass);
        static char b64[384];
        size_t olen = 0;
        mbedtls_base64_encode((unsigned char*)b64, sizeof(b64), &olen,
                              (const unsigned char*)auth_raw, strlen(auth_raw));
        b64[olen] = '\0';
        snprintf(authLine, sizeof(authLine), "Authorization: Basic %s\r\n", b64);
    }

    static char request[1024];
    int reqLen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "%s"
        "Connection: close\r\n"
        "User-Agent: M5Paper/1.0\r\n"
        "\r\n",
        path, host, authLine);

    client.write((uint8_t*)request, reqLen);
    Serial.printf("HTTP request sent (%d bytes), waiting for response...\n", reqLen);

    // ── レスポンスステータス行読み取り ──
    static char statusLine[128];
    int sl = readHeaderLine(&client, statusLine, sizeof(statusLine));
    if (sl <= 0) {
        Serial.printf("HTTP no response (heap:%d maxBlock:%d)\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        client.stop();
        return -1;
    }

    // "HTTP/1.x 200 OK" をチェック
    int httpCode = 0;
    const char* codeStart = strchr(statusLine, ' ');
    if (codeStart) httpCode = atoi(codeStart + 1);

    if (httpCode != 200) {
        Serial.printf("HTTP error: %d (%s) heap:%d maxBlock:%d\n",
                      httpCode, statusLine,
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        client.stop();
        return -1;
    }

    // ── ヘッダースキップ (空行まで読み飛ばし) ──
    static char hdr[256];
    while (true) {
        int hl = readHeaderLine(&client, hdr, sizeof(hdr));
        if (hl <= 0) break;
    }
    Serial.printf("HTTP OK, headers done (heap: %d)\n", ESP.getFreeHeap());

    // ── ICSボディをストリーミング解析（events[]にアペンド） ──
    int result = parseICSStream(&client);
    client.stop();
    Serial.printf("SSL cleanup done (heap:%d maxBlock:%d stack_free:%d)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                  uxTaskGetStackHighWaterMark(NULL));

    int added = event_count - count_before;
    if (added > 0) {
        return added;
    }

    // このURLからイベント追加なし
    return (result >= 0) ? 0 : -1;
}


bool fetchAndUpdate() {
    Serial.printf("Fetching ICS...%s (heap:%d maxBlock:%d WiFi:%d RSSI:%d fails:%d events:%d)\n",
                  debug_fetch ? " [DEBUG 30s]" : "",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.status(), WiFi.RSSI(),
                  fetch_fail_count, event_count);

    time_t now_check = time(nullptr);
    if (now_check < 1700000000) {
        Serial.printf("SKIP ICS fetch - time not synced yet (%ld)\n", now_check);
        return false;
    }

    if (strlen(config.ics_url) == 0) {
        Serial.println("ICS URL not configured");
        return false;
    }
    if (ESP.getFreeHeap() < MIN_HEAP_FOR_FETCH) {
        Serial.printf("SKIP ICS fetch - heap too low: %d < %d\n",
                      ESP.getFreeHeap(), MIN_HEAP_FOR_FETCH);
        last_fetch = time(nullptr);
        fetch_fail_count++;
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, skipping ICS fetch");
        last_fetch = time(nullptr);
        fetch_fail_count++;
        return false;
    }

    // ── ダブルバッファ切り替え（1回だけ） ──
    EventItem* prev_buf = events;
    int prev_count = event_count;
    EventItem* next_buf = (events == events_buf_a) ? events_buf_b : events_buf_a;
    events = next_buf;
    event_count = 0;
    Serial.printf("Fetch: switching to buffer %s (prev: %d events)\n",
                  (next_buf == events_buf_a) ? "A" : "B", prev_count);

    // ── カンマ区切りで複数URL順次フェッチ ──
    static char url_buf[512];
    strlcpy(url_buf, config.ics_url, sizeof(url_buf));

    int total_added = 0;
    int url_count = 0;
    int fail_count = 0;

    char* saveptr = nullptr;
    char* token = strtok_r(url_buf, ",", &saveptr);
    while (token) {
        // 前後の空白をトリム
        while (*token == ' ') token++;
        char* end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        if (strlen(token) > 0) {
            url_count++;

            // URL間ヒープチェック: SSL接続に~45KB必要。不足ならスキップ
            if (url_count > 1) {
                size_t mb_between = ESP.getMaxAllocHeap();
                if (mb_between < 50000) {
                    Serial.printf("Skipping URL %d - maxBlock %d < 50KB (need ~45KB for SSL)\n",
                                  url_count, mb_between);
                    url_count--;  // このURLはカウントしない
                    break;
                }
            }

            Serial.printf("=== Fetching URL %d: %.60s... ===\n", url_count, token);
            int result = doFetchURL(token);
            if (result > 0) {
                total_added += result;
                Serial.printf("URL %d: +%d events (total: %d)\n", url_count, result, event_count);
            } else if (result == 0) {
                Serial.printf("URL %d: 0 events added\n", url_count);
            } else {
                fail_count++;
                Serial.printf("URL %d: fetch failed\n", url_count);
            }
        }
        token = strtok_r(nullptr, ",", &saveptr);
    }

    Serial.printf("All URLs done: %d URLs, %d total events, %d failures\n",
                  url_count, event_count, fail_count);

    // ── 全URL失敗 → 旧バッファに復帰 ──
    if (event_count == 0 && fail_count > 0) {
        Serial.printf("All fetches failed - restoring previous %d events\n", prev_count);
        events = prev_buf;
        event_count = prev_count;

        fetch_fail_count++;
        size_t mb = ESP.getMaxAllocHeap();

        if (mb < 38000 && fetch_fail_count >= 3) {
            Serial.printf("=== %d failures + maxBlock %d < 38KB - restart requested ===\n",
                          fetch_fail_count, mb);
            safeReboot();
        } else if (fetch_fail_count >= 3) {
            int backoff_min = min((int)(fetch_fail_count - 2) * 5, 30);
            int poll_sec = (event_count == 0) ? 30 : (config.ics_poll_min * 60);
            last_fetch = time(nullptr) + (backoff_min * 60) - poll_sec;
            Serial.printf("=== Server unreachable (heap OK: maxBlock=%d) - backoff %d min ===\n",
                          mb, backoff_min);
            return false;
        }

        last_fetch = time(nullptr);
        return false;
    }

    // ── 全URLフェッチ完了後にソート＆トリム ──
    sortEvents();
    trimEventsAroundToday(config.max_events);

    fetch_fail_count = 0;
    size_t mb = ESP.getMaxAllocHeap();
    Serial.printf("Fetched %d events from %d URLs (heap:%d maxBlock:%d next:%dmin)\n",
                  event_count, url_count, ESP.getFreeHeap(), mb,
                  debug_fetch ? 0 : config.ics_poll_min);

    if (mb < 38000) {
        Serial.printf("=== maxBlock %d < 38KB - proactive restart requested ===\n", mb);
        safeReboot();
    }

    // ★ 画面表示テキスト比較
    bool display_changed = displayContentChanged();

    if (display_changed) {
        Serial.printf("Display content changed (%d->%d items)\n",
                      prev_count, event_count);
    } else if (prev_count != event_count) {
        Serial.printf("Events data changed but display unaffected (%d->%d items) - skip redraw\n",
                      prev_count, event_count);
    } else {
        bool data_changed = false;
        int cmp_count = min(prev_count, event_count);
        for (int i = 0; i < cmp_count; i++) {
            if (prev_buf[i].start != events[i].start ||
                strcmp(prev_buf[i].text, events[i].text) != 0) {
                data_changed = true;
                break;
            }
        }
        if (data_changed) {
            Serial.printf("Events data changed but display unaffected (%d->%d items) - skip redraw\n",
                          prev_count, event_count);
        } else {
            Serial.printf("Events unchanged (%d items)\n", event_count);
        }
    }
    last_fetch = time(nullptr);
    return display_changed;
}