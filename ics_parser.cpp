#include "globals.h"
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <ctype.h>

// ★ v029: ics_parser内のString完全排除 — char[]固定バッファのみ使用
//    DRAM断片化の最大原因だった動的String確保/解放を根絶

//==============================================================================
// 直前バッファ参照（registerEvent から triggered 状態を引き継ぐため）
//   fetchAndUpdate がバッファをスワップする直前に旧バッファをここへ保存し、
//   registerEvent() が同 (start, summary, alarm_time) のスロットを検索して
//   triggered フラグを引き継ぐ。
//==============================================================================
static EventItem* fetch_prev_buf = nullptr;
static int        fetch_prev_count = 0;

//==============================================================================
// ★ mbedTLS PSRAM アロケータ
//   SDKデフォルトはINTERNAL_MEM_ALLOC（内部DRAM専用 → ~50KB消費で断片化）
//   PSRAM対応アロケータに差し替えることで、SSLバッファをPSRAMに配置
//==============================================================================
// [LEAK] v038: mbedTLS側の確保バランスを観測するためのカウンタ
//   doFetchURL 開始時に delta を取り、終了時に差分を log する
static uint32_t mbed_alloc_psram_bytes    = 0;
static uint32_t mbed_alloc_psram_count    = 0;
static uint32_t mbed_alloc_internal_bytes = 0;
static uint32_t mbed_alloc_internal_count = 0;
static uint32_t mbed_free_count           = 0;

static void* psram_calloc(size_t n, size_t size) {
    // まずPSRAMから確保を試み、失敗したら内部RAMにフォールバック
    size_t total = n * size;
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        mbed_alloc_psram_bytes += total;
        mbed_alloc_psram_count++;
    } else {
        p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
        if (p) {
            mbed_alloc_internal_bytes += total;
            mbed_alloc_internal_count++;
        }
    }
    return p;
}

static void psram_free(void* ptr) {
    if (ptr) mbed_free_count++;
    heap_caps_free(ptr);
}

// [LEAK] 内部ヒープ統計を1行で出力
static void dumpHeapTag(const char* tag) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[LEAK] %s: free=%u largest=%u min_free=%u alloc_blocks=%u free_blocks=%u\n",
                  tag,
                  (unsigned)info.total_free_bytes,
                  (unsigned)info.largest_free_block,
                  (unsigned)info.minimum_free_bytes,
                  (unsigned)info.allocated_blocks,
                  (unsigned)info.free_blocks);
}

static bool mbedtls_psram_installed = false;

void installMbedTLSPsramAllocator() {
    if (!mbedtls_psram_installed) {
        mbedtls_platform_set_calloc_free(psram_calloc, psram_free);
        mbedtls_psram_installed = true;
        Serial.println("[SSL] mbedTLS allocator -> PSRAM");
    }
}

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
bool parseAlarmMarker(const char* s_raw, bool is_summary,
                      int* offsets, int& offset_count, int max_offsets,
                      bool& found,
                      char* midi_file, int midi_file_size, bool& midi_is_url,
                      int& duration_sec, int& repeat_count) {
    static char* norm = nullptr;  // PSRAM上に配置
    if (!norm) norm = (char*)ps_malloc(NORM_BUF);
    normalizeFullWidthBuf(s_raw, norm, NORM_BUF);
    const char* s = norm;
    int sLen = strlen(s);

    found = false;
    offset_count = 0;
    midi_file[0] = '\0';
    midi_is_url = false;
    duration_sec = -1;
    repeat_count = -1;

    // 重複オフセット排除のためのローカルlambda風ヘルパ
    auto pushOffset = [&](int v) {
        if (offset_count >= max_offsets) return;
        for (int k = 0; k < offset_count; k++) if (offsets[k] == v) return;
        offsets[offset_count++] = v;
    };

    // ── 統一ロジック: summary/description 問わず同じ判定 ──
    // 1) !...! ペアがあれば詳細パラメータを解析
    //    オフセットはカンマ区切りで複数指定可: !-25,-15,-5!
    // 2) 閉じペアのない単独 ! があればデフォルトオフセットでアラームON

    int searchStart = 0;
    while (searchStart < sLen) {
        const char* pPtr = strchr(s + searchStart, '!');
        if (!pPtr) break;
        int p = pPtr - s;

        const char* ePtr = strchr(s + p + 1, '!');
        if (!ePtr) {
            // 閉じ ! なし → 単独 ! → デフォルトオフセットでアラームON
            found = true;
            if (offset_count == 0) pushOffset(config.alarm_offset_default);
            return true;
        }
        int endExcl = ePtr - s;

        found = true;

        static char* content = nullptr;
        if (!content) content = (char*)ps_malloc(CONTENT_BUF);
        substrCopy(content, s, p + 1, endExcl, CONTENT_BUF);

        bool blockHasOffset = false;
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
                    if (val >= 0 && val <= 24 * 60) {
                        int signedVal = (c == '-') ? val : -val;
                        pushOffset(signedVal);
                        blockHasOffset = true;
                    }
                }
                i = numEnd;
            } else if (c == ',' || c == ' ') {
                // カンマ・空白はオフセット区切りとして読み飛ばし
                i++;
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

        // !...! ブロック内に明示オフセットが無ければデフォルトを1つ
        if (!blockHasOffset) pushOffset(config.alarm_offset_default);

        if (thisFile[0] != '\0') {
            safeCopy(midi_file, thisFile, midi_file_size);
            midi_is_url = thisIsUrl;
        }
        searchStart = endExcl + 1;
    }

    if (found && offset_count == 0) pushOffset(config.alarm_offset_default);
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
    // 過去ウィンドウ: 7日前まで取り込む
    //   trimEventsAroundToday() が過去最大10件まで表示保持するため、
    //   24h 固定だと「画面に残っているのに再fetchで取り込まれず、編集が反映されない」
    //   という状態が発生する。表示されうる過去イベントは必ず再パースする。
    if (st <= now - 7 * 86400 || st >= now + 30 * 86400) return;

    int parsed_offsets[MAX_ALARMS_PER_EVENT];
    int parsed_off_n = 0;
    bool hasAL = false;
    char midi_file_str[MIDI_FILE_BUF];
    midi_file_str[0] = '\0';
    bool midi_is_url_flag = false;
    int ev_duration = -1, ev_repeat = -1;

    parseAlarmMarker(summary, true, parsed_offsets, parsed_off_n, MAX_ALARMS_PER_EVENT,
                     hasAL, midi_file_str, MIDI_FILE_BUF,
                     midi_is_url_flag, ev_duration, ev_repeat);
    if (!hasAL && desc[0] != '\0') {
        parseAlarmMarker(desc, false, parsed_offsets, parsed_off_n, MAX_ALARMS_PER_EVENT,
                         hasAL, midi_file_str, MIDI_FILE_BUF,
                         midi_is_url_flag, ev_duration, ev_repeat);
    }

    if (hasAL) {
        char logSum[41];
        substrCopy(logSum, summary, 0, 40, sizeof(logSum));
        char offBuf[64]; offBuf[0] = '\0';
        int op = 0;
        for (int k = 0; k < parsed_off_n && op < (int)sizeof(offBuf) - 8; k++) {
            op += snprintf(offBuf + op, sizeof(offBuf) - op,
                           k == 0 ? "%d" : ",%d", parsed_offsets[k]);
        }
        Serial.printf("ICS_STREAM: [%d] ALARM '%s' offsets=[%s]\n",
                      event_count, logSum, offBuf);
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

    events[idx].alarm_count = 0;
    if (hasAL) {
        const time_t ALARM_GRACE_SEC = 600;       // 起動直後の再生防止グレース
        const time_t LATE_ADD_GRACE  = 86400;     // 通常fetch時: 開始翌日まで遅延発火を許容

        // 旧バッファに同 (start, summary) のイベントが居れば triggered を引き継ぐための検索
        const EventItem* prev_match = nullptr;
        if (fetch_prev_buf && fetch_prev_count > 0) {
            for (int p = 0; p < fetch_prev_count; p++) {
                if (fetch_prev_buf[p].start != st) continue;
                if (strcmp(fetch_prev_buf[p].summary(), events[idx].summary()) != 0) continue;
                prev_match = &fetch_prev_buf[p];
                break;
            }
        }

        for (int k = 0; k < parsed_off_n; k++) {
            int slot = events[idx].alarm_count;
            time_t at = st - (time_t)parsed_offsets[k] * 60;
            events[idx].offset_min[slot] = parsed_offsets[k];
            events[idx].alarm_time[slot] = at;

            // 1) 旧バッファに同じ alarm_time のスロットがあれば、その triggered を継承
            bool carried = false;
            if (prev_match) {
                for (int j = 0; j < prev_match->alarm_count; j++) {
                    if (prev_match->alarm_time[j] == at) {
                        events[idx].triggered[slot] = prev_match->triggered[j];
                        carried = true;
                        break;
                    }
                }
            }

            // 2) 新規アラーム: 起動初回は従来グレース、通常fetch中は「開始翌日まで」許容
            if (!carried) {
                if (!initial_fetch_done) {
                    events[idx].triggered[slot] = (at < now - ALARM_GRACE_SEC);
                } else {
                    // 後付け!でも開始時刻 + 24h までは鳴らす（既に終わった予定は抑止）
                    events[idx].triggered[slot] = (st < now - LATE_ADD_GRACE);
                }
            }
            events[idx].alarm_count++;
        }
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

    // [LEAK] doFetchURL 入口の計装 — mbedTLSアロケータの累積カウンタ diff 用スナップショット
    uint32_t mbed_psram_b0  = mbed_alloc_psram_bytes;
    uint32_t mbed_psram_c0  = mbed_alloc_psram_count;
    uint32_t mbed_intrn_b0  = mbed_alloc_internal_bytes;
    uint32_t mbed_intrn_c0  = mbed_alloc_internal_count;
    uint32_t mbed_free_c0   = mbed_free_count;
    dumpHeapTag("doFetchURL:enter");

    // ── SSL接続 ──
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);
    Serial.printf("SSL client initialized (heap: %d, maxBlock: %d)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    dumpHeapTag("WiFiClientSecure:after_init");

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

    // ── キャッシュバイパス用タイムスタンプ付きパス ──
    static char path_nocache[300];
    {
        // URL に既存の '?' があるかチェック
        const char* qmark = strchr(path, '?');
        snprintf(path_nocache, sizeof(path_nocache), "%s%c_t=%ld",
                 path, qmark ? '&' : '?', (long)time(nullptr));
    }

    static char request[1024];
    int reqLen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "%s"
        "Cache-Control: no-cache, no-store\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "User-Agent: M5Paper/1.0\r\n"
        "\r\n",
        path_nocache, host, authLine);

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
    dumpHeapTag("parseICSStream:before");

    // ── ICSボディをストリーミング解析（events[]にアペンド） ──
    int result = parseICSStream(&client);
    dumpHeapTag("parseICSStream:after");
    client.stop();
    dumpHeapTag("client.stop:after");
    Serial.printf("SSL cleanup done (heap:%d maxBlock:%d stack_free:%d)\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                  uxTaskGetStackHighWaterMark(NULL));
    // [LEAK] doFetchURL 出口の diff 表示 — このcycleでPSRAM/internalに何バイト新規確保され、
    //        いくつ解放されなかったかを可視化する
    Serial.printf("[LEAK] doFetchURL:exit mbed_psram=+%u/%u mbed_intrn=+%u/%u free_diff=%d (alloc_tot=%u free_tot=%u)\n",
                  (unsigned)(mbed_alloc_psram_bytes - mbed_psram_b0),
                  (unsigned)(mbed_alloc_psram_count - mbed_psram_c0),
                  (unsigned)(mbed_alloc_internal_bytes - mbed_intrn_b0),
                  (unsigned)(mbed_alloc_internal_count - mbed_intrn_c0),
                  (int)((mbed_alloc_psram_count + mbed_alloc_internal_count - mbed_psram_c0 - mbed_intrn_c0)
                        - (mbed_free_count - mbed_free_c0)),
                  (unsigned)(mbed_alloc_psram_count + mbed_alloc_internal_count),
                  (unsigned)mbed_free_count);

    int added = event_count - count_before;
    if (added > 0) {
        return added;
    }

    // このURLからイベント追加なし
    return (result >= 0) ? 0 : -1;
}


bool fetchAndUpdate() {
    // ★ mbedTLSのメモリ確保先をPSRAMに変更（初回のみ）
    installMbedTLSPsramAllocator();

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
    // registerEvent から旧バッファを参照できるよう公開
    fetch_prev_buf   = prev_buf;
    fetch_prev_count = prev_count;
    Serial.printf("Fetch: switching to buffer %s (prev: %d events)\n",
                  (next_buf == events_buf_a) ? "A" : "B", prev_count);

    // ── カンマ区切りで複数URL順次フェッチ ──
    static char url_buf[512];
    strlcpy(url_buf, config.ics_url, sizeof(url_buf));

    int total_added = 0;
    int url_count = 0;
    int fail_count = 0;
    int skip_count = 0;
    int total_urls = 0;

    // まず総URL数をカウント
    {
        char count_buf[512];
        strlcpy(count_buf, config.ics_url, sizeof(count_buf));
        char* sp = nullptr;
        char* tk = strtok_r(count_buf, ",", &sp);
        while (tk) {
            while (*tk == ' ') tk++;
            if (strlen(tk) > 0) total_urls++;
            tk = strtok_r(nullptr, ",", &sp);
        }
    }
    Serial.printf("ICS URLs configured: %d\n", total_urls);

    // ── URLごとの状態テーブルを初期化 (ヘッダー表示用) ──
    fetch_url_count = (total_urls > MAX_FETCH_URLS) ? MAX_FETCH_URLS : total_urls;
    for (int i = 0; i < MAX_FETCH_URLS; i++) fetch_url_status[i] = 0;

    char* saveptr = nullptr;
    char* token = strtok_r(url_buf, ",", &saveptr);
    while (token) {
        // 前後の空白をトリム
        while (*token == ' ') token++;
        char* end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        if (strlen(token) > 0) {
            url_count++;

            // URL間ヒープチェック
            // ★ PSRAMアロケータ有効時はSSLバッファがPSRAMに行くため閾値を大幅引き下げ
            //    フォールバック: 内部ヒープ枯渇時のみWiFi再接続で回復を試みる
            if (url_count > 1) {
                size_t mb_between = ESP.getMaxAllocHeap();
                size_t heap_between = ESP.getFreeHeap();
                Serial.printf("URL %d: pre-check heap=%d maxBlock=%d psram=%dKB\n",
                              url_count, heap_between, mb_between, ESP.getFreePsram() / 1024);
                if (heap_between < 40000) {
                    // 内部ヒープ自体が40KB未満 → WiFi再接続で回復試行
                    Serial.printf("URL %d: heap %d < 40KB - WiFi restart to recover...\n",
                                  url_count, heap_between);
                    WiFi.disconnect(true);
                    delay(200);
                    if (!connectWiFi()) {
                        Serial.println("WiFi reconnect failed");
                        skip_count++;
                        if (url_count - 1 < MAX_FETCH_URLS) fetch_url_status[url_count - 1] = 2;
                        char* remaining = strtok_r(nullptr, ",", &saveptr);
                        int skip_idx = url_count;
                        while (remaining) {
                            while (*remaining == ' ') remaining++;
                            if (strlen(remaining) > 0) {
                                skip_count++;
                                if (skip_idx < MAX_FETCH_URLS) fetch_url_status[skip_idx] = 2;
                                skip_idx++;
                            }
                            remaining = strtok_r(nullptr, ",", &saveptr);
                        }
                        break;
                    }
                    Serial.printf("After WiFi restart: heap=%d maxBlock=%d\n",
                                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
                }
            }

            Serial.printf("=== Fetching URL %d/%d: %.60s... ===\n", url_count, total_urls, token);
            dumpHeapTag("loop:before_doFetchURL");
            int result = doFetchURL(token);
            // [LEAK] ここは WiFiClientSecure destructor 実行後の状態
            dumpHeapTag("loop:after_doFetchURL+dtor");
            if (result >= 0) {
                if (url_count - 1 < MAX_FETCH_URLS) fetch_url_status[url_count - 1] = 1;
                if (result > 0) {
                    total_added += result;
                    Serial.printf("URL %d: +%d events (total: %d)\n", url_count, result, event_count);
                } else {
                    Serial.printf("URL %d: 0 events added\n", url_count);
                }
            } else {
                fail_count++;
                if (url_count - 1 < MAX_FETCH_URLS) fetch_url_status[url_count - 1] = 2;
                Serial.printf("URL %d: fetch failed\n", url_count);
            }
        }
        token = strtok_r(nullptr, ",", &saveptr);
    }

    Serial.printf("All URLs done: %d/%d fetched, %d failed, %d skipped, %d events\n",
                  url_count - fail_count, total_urls, fail_count, skip_count, event_count);

    // ── URL失敗/スキップあり → 部分データ採用（ゼロ件のときだけ旧データ復帰） ──
    // 旧仕様: event_count < prev_count なら一律旧データへ復帰
    //   → 1本のURLが失敗し続けると起動時スナップショットから永久に更新されない
    //     (新規追加した予定が再起動まで見えないバグ v036 で修正)
    // 新仕様: 部分成功した新データを採用。次サイクルで失敗URLは再取得される。
    bool incomplete_fetch = (fail_count > 0 || skip_count > 0);
    if (incomplete_fetch && event_count == 0) {
        Serial.printf("All fetches failed/skipped - restoring previous %d events\n", prev_count);
        events = prev_buf;
        event_count = prev_count;
        fetch_fail_count++;
        size_t mb = ESP.getMaxAllocHeap();

        if (mb < 20000 && fetch_fail_count >= 3) {
            Serial.printf("=== %d failures + maxBlock %d < 20KB - restart requested ===\n",
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

        if (skip_count > 0) {
            heap_skip_count++;
            Serial.printf("=== URLs skipped due to heap (skip streak: %d) ===\n", heap_skip_count);
            if (heap_skip_count >= 3) {
                Serial.println("=== Too many heap skips - proactive restart ===");
                heap_skip_count = 0;
                safeReboot();
            }
        }

        last_fetch = time(nullptr);
        return false;
    }
    if (incomplete_fetch) {
        Serial.printf("*** Partial fetch (fail:%d skip:%d) — accepting %d events (prev:%d) ***\n",
                      fail_count, skip_count, event_count, prev_count);
        // v038: ~35KB/cycle の heap leak があり、数サイクル後に URL2/3 の SSL connect が
        //       落ちる（= fch2X fch3X が常時表示される）。WiFi が生きているなら原因は
        //       heap とみなしてサイレント再起動で heap を再生する。WiFi 切断中は
        //       ネットワーク障害の可能性があるので reboot loop 防止のため見送る。
        //       e-paper は残像保持なので pushCanvas せずに restart しても画面は前の
        //       状態を維持し、再起動後の fetch で自然に更新される。
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("=== Partial fetch with WiFi up — suspect heap leak, silent reboot ===");
            safeReboot();
        }
    }

    // ── 全URLフェッチ完了後にソート＆トリム ──
    sortEvents();
    trimEventsAroundToday(config.max_events);

    fetch_fail_count = 0;
    heap_skip_count = 0;
    size_t mb = ESP.getMaxAllocHeap();
    Serial.printf("Fetched %d events from %d URLs (heap:%d maxBlock:%d next:%dmin)\n",
                  event_count, url_count, ESP.getFreeHeap(), mb,
                  debug_fetch ? 0 : config.ics_poll_min);

    // ★ アラーム状態サマリー（データ更新確認用）
    {
        int alarm_count = 0;
        for (int i = 0; i < event_count; i++) {
            if (events[i].has_alarm) {
                alarm_count++;
                struct tm st; localtime_r(&events[i].start, &st);
                char offBuf[96]; offBuf[0] = '\0';
                int op = 0;
                for (int k = 0; k < events[i].alarm_count && op < (int)sizeof(offBuf) - 12; k++) {
                    op += snprintf(offBuf + op, sizeof(offBuf) - op,
                                   k == 0 ? "%d%s" : ",%d%s",
                                   events[i].offset_min[k],
                                   events[i].triggered[k] ? "*" : "");
                }
                Serial.printf("  ALARM[%d]: %02d/%02d %02d:%02d '%s' off=[%s]min\n",
                              i, st.tm_mon+1, st.tm_mday, st.tm_hour, st.tm_min,
                              events[i].summary(), offBuf);
            }
        }
        Serial.printf("  Total alarms: %d / %d events\n", alarm_count, event_count);
    }

    if (mb < 38000) {
        Serial.printf("=== maxBlock %d < 38KB - proactive restart requested ===\n", mb);
        safeReboot();
    }

    // ★ フェッチ成功時は常に再描画する（変更検出は不正確で「!」追加等を見逃すため）
    //    定期fetchは数分に1回なのでGC16フラッシュ頻度は許容範囲
    Serial.printf("Fetch complete (%d->%d items) - always redraw\n",
                  prev_count, event_count);
    last_fetch = time(nullptr);
    initial_fetch_done = true;          // 以降の fetch は「通常fetch」扱い
    fetch_prev_buf = nullptr;
    fetch_prev_count = 0;
    return true;
}