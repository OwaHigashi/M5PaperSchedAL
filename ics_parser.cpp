#include "globals.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <base64.h>
#include <time.h>
#include <SD.h>

//==============================================================================
// 日時パース
//==============================================================================
bool parseDT(const String& raw, time_t& out, bool& is_allday) {
    String s = raw;
    s.trim();
    bool utc = s.endsWith("Z");
    if (utc) s.remove(s.length() - 1);

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    is_allday = false;

    if (s.length() == 8) {
        y  = s.substring(0, 4).toInt();
        mo = s.substring(4, 6).toInt();
        d  = s.substring(6, 8).toInt();
        is_allday = true;
    } else if (s.length() >= 15 && s[8] == 'T') {
        y  = s.substring(0, 4).toInt();
        mo = s.substring(4, 6).toInt();
        d  = s.substring(6, 8).toInt();
        h  = s.substring(9, 11).toInt();
        mi = s.substring(11, 13).toInt();
        se = s.substring(13, 15).toInt();
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
bool parseAlarmMarker(const String& s_raw, bool is_summary, int& off, bool& found,
                      String& midi_file, bool& midi_is_url,
                      int& duration_sec, int& repeat_count) {
    String s = normalizeFullWidth(s_raw);

    found = false;
    off = config.alarm_offset_default;
    midi_file = "";
    midi_is_url = false;
    duration_sec = -1;
    repeat_count = -1;
    int maxOff = -1;

    if (is_summary) {
        int exclamCount = 0;
        for (int i = 0; i < (int)s.length(); i++) {
            if (s[i] == '!') exclamCount++;
        }
        if (exclamCount == 1) {
            found = true;
            return true;
        }
    }

    int searchStart = 0;
    while (searchStart < (int)s.length()) {
        int p = s.indexOf('!', searchStart);
        if (p < 0) break;

        int endExcl = s.indexOf('!', p + 1);
        if (endExcl < 0) {
            if (is_summary) { found = true; return true; }
            break;
        }

        found = true;
        String content = s.substring(p + 1, endExcl);

        int thisOff = config.alarm_offset_default;
        bool thisIsUrl = false;
        String thisFile = "";

        int i = 0;
        while (i < (int)content.length()) {
            char c = content[i];
            if (c == '-' || c == '+') {
                int numStart = i + 1, numEnd = numStart;
                while (numEnd < (int)content.length() &&
                       (isdigit(content[numEnd]) || content[numEnd] == ' ')) numEnd++;
                if (numEnd > numStart) {
                    String num = content.substring(numStart, numEnd); num.trim();
                    int val = num.toInt();
                    if (val >= 0 && val <= 24 * 60) thisOff = (c == '-') ? val : -val;
                }
                i = numEnd;
            } else if (c == '>' || c == '<') {
                thisIsUrl = (c == '>');
                int fileStart = i + 1, fileEnd = fileStart;
                while (fileEnd < (int)content.length() &&
                       content[fileEnd] != '-' && content[fileEnd] != '+' &&
                       content[fileEnd] != '@' && content[fileEnd] != '*' &&
                       content[fileEnd] != '>' && content[fileEnd] != '<') fileEnd++;
                if (fileEnd > fileStart) {
                    thisFile = content.substring(fileStart, fileEnd); thisFile.trim();
                }
                i = fileEnd;
            } else if (c == '@') {
                int numStart = i + 1, numEnd = numStart;
                while (numEnd < (int)content.length() &&
                       (isdigit(content[numEnd]) || content[numEnd] == ' ')) numEnd++;
                if (numEnd > numStart) {
                    String num = content.substring(numStart, numEnd); num.trim();
                    duration_sec = num.toInt();
                } else {
                    duration_sec = 0;
                }
                i = numEnd;
            } else if (c == '*') {
                int numStart = i + 1, numEnd = numStart;
                while (numEnd < (int)content.length() &&
                       (isdigit(content[numEnd]) || content[numEnd] == ' ')) numEnd++;
                if (numEnd > numStart) {
                    String num = content.substring(numStart, numEnd); num.trim();
                    repeat_count = num.toInt();
                }
                i = numEnd;
            } else {
                i++;
            }
        }

        if (abs(thisOff) > abs(maxOff) || maxOff == -1) maxOff = thisOff;
        if (thisFile.length() > 0) { midi_file = thisFile; midi_is_url = thisIsUrl; }
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
            if (i < start_idx) {
                events[i].summary = "";
                events[i].description = "";
                events[i].midi_file = "";
            }
            events[i] = events[i + start_idx];
        }
        event_count -= start_idx;
    }

    if (event_count > maxEvents) {
        for (int i = maxEvents; i < event_count; i++) {
            events[i].summary = "";
            events[i].description = "";
            events[i].midi_file = "";
        }
        event_count = maxEvents;
    }
    Serial.printf("TRIM: result=%d events\n", event_count);
}

//==============================================================================
// イベントキャッシュ（SD保存/読込）
//==============================================================================

// キャッシュ保存 — 取得成功時にSDへ書き出し
void saveEventsCache() {
    if (event_count <= 0 || !sd_healthy) return;
    waitEPDReady();

    if (!SD.exists("/cache")) SD.mkdir("/cache");

    File f = SD.open(EVENTS_CACHE_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("CACHE: Failed to open for write");
        return;
    }

    f.printf("V1\n%d\n", event_count);
    for (int i = 0; i < event_count; i++) {
        // メタデータ行: start|allday|alarm|offset|alarm_time|triggered|dur|rep|midi_url
        f.printf("%ld|%d|%d|%d|%ld|%d|%d|%d|%d\n",
                 (long)events[i].start,
                 events[i].is_allday ? 1 : 0,
                 events[i].has_alarm ? 1 : 0,
                 events[i].offset_min,
                 (long)events[i].alarm_time,
                 events[i].triggered ? 1 : 0,
                 events[i].play_duration_sec,
                 events[i].play_repeat,
                 events[i].midi_is_url ? 1 : 0);
        // summary（改行→\nエスケープ）
        String s = events[i].summary;
        s.replace("\r", ""); s.replace("\n", "\\n");
        f.print(s); f.print('\n');
        // description
        String d = events[i].description;
        d.replace("\r", ""); d.replace("\n", "\\n");
        f.print(d); f.print('\n');
        // midi_file
        f.print(events[i].midi_file); f.print('\n');
    }
    f.flush();
    f.close();
    Serial.printf("CACHE: Saved %d events to SD\n", event_count);
}

// キャッシュ読込 — HTTP失敗時やコールドスタート時
bool loadEventsCache() {
    if (!sd_healthy) return false;
    waitEPDReady();
    if (!SD.exists(EVENTS_CACHE_FILE)) {
        Serial.println("CACHE: No cache file");
        return false;
    }

    File f = SD.open(EVENTS_CACHE_FILE, FILE_READ);
    if (!f) {
        Serial.println("CACHE: Failed to open for read");
        return false;
    }

    String header = f.readStringUntil('\n');
    header.trim();
    if (header != "V1") {
        Serial.printf("CACHE: Invalid header '%s'\n", header.c_str());
        f.close();
        return false;
    }

    String countStr = f.readStringUntil('\n');
    countStr.trim();
    int cached_count = countStr.toInt();
    if (cached_count <= 0 || cached_count > MAX_EVENTS) {
        Serial.printf("CACHE: Invalid count %d\n", cached_count);
        f.close();
        return false;
    }

    // 現在のイベントをクリア
    for (int i = 0; i < event_count; i++) {
        events[i].summary = "";
        events[i].description = "";
        events[i].midi_file = "";
    }
    event_count = 0;

    time_t now = time(nullptr);
    const time_t ALARM_GRACE_SEC = 600;

    for (int i = 0; i < cached_count; i++) {
        if (!f.available()) break;

        // メタデータ行
        String meta = f.readStringUntil('\n');
        meta.trim();
        if (meta.length() == 0) break;

        long v_start = 0, v_alarm_time = 0;
        int v_allday = 0, v_alarm = 0, v_offset = 0, v_triggered = 0;
        int v_dur = -1, v_rep = -1, v_midi_url = 0;

        int fields = sscanf(meta.c_str(), "%ld|%d|%d|%d|%ld|%d|%d|%d|%d",
                            &v_start, &v_allday, &v_alarm, &v_offset,
                            &v_alarm_time, &v_triggered, &v_dur, &v_rep, &v_midi_url);

        String summary = f.readStringUntil('\n');
        summary.replace("\r", "");
        summary.replace("\\n", "\n");
        String desc = f.readStringUntil('\n');
        desc.replace("\r", "");
        desc.replace("\\n", "\n");
        String midi = f.readStringUntil('\n');
        midi.trim();

        if (fields < 9) continue;

        // 1日以上前のイベントをスキップ
        if ((time_t)v_start <= now - 86400) continue;

        int idx = event_count;
        events[idx].start = (time_t)v_start;
        events[idx].is_allday = (v_allday != 0);
        events[idx].has_alarm = (v_alarm != 0);
        events[idx].offset_min = v_offset;
        events[idx].alarm_time = (time_t)v_alarm_time;
        events[idx].play_duration_sec = v_dur;
        events[idx].play_repeat = v_rep;
        events[idx].midi_is_url = (v_midi_url != 0);
        events[idx].summary = summary;
        events[idx].description = desc;
        events[idx].midi_file = midi;

        // triggered状態を再計算
        if (events[idx].has_alarm) {
            events[idx].triggered = (events[idx].alarm_time < now - ALARM_GRACE_SEC);
        } else {
            events[idx].triggered = false;
        }

        event_count++;
        if (event_count >= MAX_EVENTS) break;
    }

    f.close();
    Serial.printf("CACHE: Loaded %d events from SD (heap: %d)\n",
                  event_count, ESP.getFreeHeap());
    return event_count > 0;
}

//==============================================================================
// ストリーミングICSパーサー
//==============================================================================

// ストリームから1行読み取り（CRLF/LF区切り）
static bool readRawLine(WiFiClient* stream, String& line, int maxLen = 2048) {
    line = "";
    unsigned long timeout = millis() + 10000;

    while (stream->connected() || stream->available()) {
        if (millis() > timeout) {
            Serial.println("ICS_STREAM: readline timeout");
            return line.length() > 0;
        }
        if (!stream->available()) { delay(1); continue; }

        char c = stream->read();
        if (c == '\r') continue;
        if (c == '\n') return true;
        if ((int)line.length() < maxLen) line += c;
    }
    return line.length() > 0;
}

// RFC 5545 unfold処理統合 — 次行が空白/タブで始まる場合は結合
static bool readUnfoldedLine(WiFiClient* stream, String& line, String& pushback) {
    if (pushback.length() > 0) {
        line = pushback;
        pushback = "";
    } else {
        if (!readRawLine(stream, line)) return false;
    }

    while (stream->connected() || stream->available()) {
        String nextLine;
        if (!readRawLine(stream, nextLine)) break;

        if (nextLine.length() > 0 && (nextLine[0] == ' ' || nextLine[0] == '\t')) {
            if ((int)line.length() < 4096) line += nextLine.substring(1);
        } else {
            pushback = nextLine;
            return true;
        }
    }
    return true;
}

// 1つのVEVENTをevents[]に登録
static void registerEvent(const String& dtstart_raw, const String& summary, const String& desc) {
    if (event_count >= MAX_EVENTS) return;

    time_t now = time(nullptr);
    time_t st = 0;
    bool is_allday = false;
    if (!parseDT(dtstart_raw, st, is_allday)) return;
    if (st <= now - 86400 || st >= now + 30 * 86400) return;

    if (ESP.getFreeHeap() < (size_t)config.min_free_heap * 1024) {
        Serial.printf("ICS_STREAM: STOP - heap low (%d < %dKB), %d events loaded\n",
                      ESP.getFreeHeap(), config.min_free_heap, event_count);
        return;
    }

    int off = 0;
    bool hasAL = false;
    String midi_file_str = "";
    bool midi_is_url_flag = false;
    int ev_duration = -1, ev_repeat = -1;

    parseAlarmMarker(summary, true, off, hasAL, midi_file_str, midi_is_url_flag,
                     ev_duration, ev_repeat);
    if (!hasAL && desc.length() > 0) {
        parseAlarmMarker(desc, false, off, hasAL, midi_file_str, midi_is_url_flag,
                         ev_duration, ev_repeat);
    }

    if (hasAL) {
        Serial.printf("ICS_STREAM: [%d] ALARM '%s' offset=%d\n",
                      event_count, summary.substring(0, 40).c_str(), off);
    }

    int idx = event_count;
    events[idx].start = st;
    events[idx].summary = summary;

    String trimmedDesc = desc;
    if ((int)trimmedDesc.length() > config.max_desc_bytes) {
        int cutAt = config.max_desc_bytes;
        while (cutAt > 0 && ((uint8_t)trimmedDesc[cutAt] & 0xC0) == 0x80) cutAt--;
        trimmedDesc = trimmedDesc.substring(0, cutAt);
    }
    events[idx].description = trimmedDesc;
    events[idx].has_alarm = hasAL;
    events[idx].is_allday = is_allday;
    events[idx].midi_file = midi_file_str;
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

// ストリーミングパーサー本体（安全版 v028）
// 旧データを先にクリアせず、パース完了後に件数を検証してから置き換える
static int parseICSStream(WiFiClient* stream, int old_event_count) {
    // 一時バッファ: events[]の後半（MAX_EVENTS/2〜）に仮置きしてから前半に移動
    // ただしメモリ効率のため、直接events[]に上書きし、失敗時はキャッシュから復旧する
    // → 呼び出し元(fetchAndUpdate)が事前にキャッシュ保存済みなので安全

    for (int i = 0; i < event_count; i++) {
        events[i].summary = "";
        events[i].description = "";
        events[i].midi_file = "";
    }
    event_count = 0;

    bool inEvent = false;
    String dtstart_raw, summary, desc;
    String pushback = "";
    int parsed_events = 0;
    String line;

    Serial.printf("ICS_STREAM: Start parsing (heap: %d)\n", ESP.getFreeHeap());

    while (readUnfoldedLine(stream, line, pushback)) {
        line.trim();
        if (!line.length()) continue;

        if (line == "BEGIN:VEVENT") {
            inEvent = true;
            dtstart_raw = ""; summary = ""; desc = "";
            continue;
        }

        if (line == "END:VEVENT") {
            parsed_events++;
            if (inEvent) {
                registerEvent(dtstart_raw, summary, desc);
                if (event_count >= MAX_EVENTS) {
                    Serial.println("ICS_STREAM: MAX_EVENTS reached");
                    break;
                }
            }
            inEvent = false;
            dtstart_raw = ""; summary = ""; desc = "";
            continue;
        }

        if (!inEvent) continue;

        if (line.startsWith("DTSTART")) {
            int c = line.indexOf(':');
            if (c > 0) dtstart_raw = line.substring(c + 1);
        } else if (line.startsWith("SUMMARY")) {
            int c = line.indexOf(':');
            if (c > 0) summary = line.substring(c + 1);
        } else if (line.startsWith("DESCRIPTION")) {
            int c = line.indexOf(':');
            if (c > 0) {
                int maxParseLen = 2000;
                String full = line.substring(c + 1);
                desc = ((int)full.length() > maxParseLen) ? full.substring(0, maxParseLen) : full;
                full = "";
            }
        }
    }

    Serial.printf("ICS_STREAM: Complete - parsed %d VEVENTs, loaded %d (was %d) (heap: %d)\n",
                  parsed_events, event_count, old_event_count, ESP.getFreeHeap());

    // ============ 安全チェック ============
    // (1) 新データ0件 → 明らかに失敗
    if (event_count == 0 && old_event_count > 0) {
        Serial.printf("ICS_STREAM: REJECT - 0 events (was %d). Stream error.\n",
                      old_event_count);
        return -1;  // キャッシュから復旧
    }

    // (2) 新データが旧データの半分未満 → 途中切れの疑い
    if (old_event_count >= 10 && event_count < old_event_count / 2) {
        Serial.printf("ICS_STREAM: REJECT - partial load %d (was %d). "
                      "Possible stream cutoff.\n", event_count, old_event_count);
        return -1;  // キャッシュから復旧
    }

    // (3) VEVENTを100件以上パースしたのに日付フィルタ通過が極端に少ない → 時刻異常の疑い
    if (parsed_events > 100 && event_count < 3) {
        Serial.printf("ICS_STREAM: REJECT - parsed %d but only %d passed filter. "
                      "Possible time issue.\n", parsed_events, event_count);
        return -1;
    }

    sortEvents();
    trimEventsAroundToday(config.max_events);
    return event_count;
}

//==============================================================================
// ICS取得 + ストリーミング解析
//==============================================================================
void fetchAndUpdate() {
    Serial.printf("Fetching ICS... (heap: %d)\n", ESP.getFreeHeap());

    // 時刻未同期チェック（NTP未反映だと日付フィルタで全件却下される）
    time_t now_check = time(nullptr);
    if (now_check < 1700000000) {
        Serial.printf("SKIP ICS fetch - time not synced yet (%ld)\n", now_check);
        return;
    }

    if (strlen(config.ics_url) == 0) {
        Serial.println("ICS URL not configured");
        return;
    }
    if (ESP.getFreeHeap() < MIN_HEAP_FOR_FETCH) {
        Serial.printf("SKIP ICS fetch - heap too low: %d < %d\n",
                      ESP.getFreeHeap(), MIN_HEAP_FOR_FETCH);
        last_fetch = time(nullptr);
        fetch_fail_count++;
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, skipping ICS fetch");
        last_fetch = time(nullptr);
        fetch_fail_count++;
        return;
    }

    // ★ フェッチ前にキャッシュをバックアップ（ロールバック用）
    int old_count = event_count;
    if (old_count > 0 && sd_healthy) {
        saveEventsCache();
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(10000);

    if (!http.begin(client, config.ics_url)) {
        Serial.println("HTTP begin failed");
        last_fetch = time(nullptr);
        fetch_fail_count++;
        return;
    }

    if (strlen(config.ics_user) > 0) {
        String auth = String(config.ics_user) + ":" + String(config.ics_pass);
        String authHeader = "Basic " + base64::encode(auth);
        http.addHeader("Authorization", authHeader);
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("HTTP GET failed: %d (WiFi:%d)\n", code, WiFi.status());
        Serial.printf("  URL: %.60s...\n", config.ics_url);
        Serial.printf("  heap: %d\n", ESP.getFreeHeap());
        if (code == -1) {
            Serial.println("  -> Connection refused/timeout (SSL/DNS issue)");
        }
        http.end();

        // HTTP失敗でイベント0件ならキャッシュから復旧
        if (event_count == 0) {
            Serial.println("  -> No events in memory, trying cache...");
            delay(10);
            loadEventsCache();
        }

        last_fetch = time(nullptr);
        fetch_fail_count++;
        return;
    }

    Serial.printf("HTTP OK, content-length: %d, heap: %d\n",
                  http.getSize(), ESP.getFreeHeap());

    WiFiClient* stream = http.getStreamPtr();
    int result = parseICSStream(stream, old_count);

    http.end();

    // ★ ネットワーク終了後、SD操作前にSPI安定化delay
    delay(10);

    if (result > 0) {
        // 取得成功＋検証OK → キャッシュ更新
        saveEventsCache();
        fetch_fail_count = 0;
        Serial.printf("Fetched %d events (heap: %d, next in %d min)\n",
                      event_count, ESP.getFreeHeap(), config.ics_poll_min);
    } else if (result == -1) {
        // 検証NG → キャッシュからロールバック
        Serial.printf("ICS data rejected (got %d, was %d), rolling back from cache...\n",
                      event_count, old_count);
        loadEventsCache();
        fetch_fail_count++;
    } else {
        // 0件（初回取得など）→ キャッシュからリカバリ試行
        Serial.println("ICS parse returned 0 events, trying cache...");
        loadEventsCache();
        fetch_fail_count++;
    }

    Serial.printf("fetchAndUpdate done: %d events, heap: %d\n",
                  event_count, ESP.getFreeHeap());
    last_fetch = time(nullptr);
}
