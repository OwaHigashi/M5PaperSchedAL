#include "globals.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

//==============================================================================
// ホスト同期クライアント
//   - GET  /api/v1/events    : NDJSON (ヘッダ行 + 1イベント1行) を1行ずつ PSRAM で解析
//   - POST /api/v1/heartbeat : Active sensing。時刻合わせ・テーブル版・コマンド受信
//   - POST /api/v1/alarm/ack : 鳴動完了。ホストが triggered を確定する
//   ホストが真実。端末は表示と鳴動だけを担当し、自分の状態は常に報告する。
//==============================================================================

// ArduinoJson を PSRAM に置くアロケータ
struct SpiRamAllocator {
    void* allocate(size_t size) { return ps_malloc(size); }
    void deallocate(void* p) { free(p); }
    void* reallocate(void* p, size_t n) { return ps_realloc(p, n); }
};
using SpiRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;

static const size_t LINE_BUF_SIZE = 24 * 1024;   // 1行 (summary+desc 最大 ~4KB + エスケープ余裕)
static const size_t DOC_SIZE      = 48 * 1024;
static const size_t RESP_BUF_SIZE = 8 * 1024;

static char* line_buf = nullptr;
static char* resp_buf = nullptr;
static char* body_buf = nullptr;
static SpiRamJsonDocument* doc = nullptr;

static void ensureBuffers() {
    if (!line_buf) line_buf = (char*)ps_malloc(LINE_BUF_SIZE);
    if (!resp_buf) resp_buf = (char*)ps_malloc(RESP_BUF_SIZE);
    if (!body_buf) body_buf = (char*)ps_malloc(RESP_BUF_SIZE);
    if (!doc) doc = new SpiRamJsonDocument(DOC_SIZE);
}

//------------------------------------------------------------------------------
// 未達 ACK (ホストに届くまで LittleFS に保持、毎ハートビートで再送)
//------------------------------------------------------------------------------
static char pending_ack[MAX_PENDING_ACKS][40];
static char pending_res[MAX_PENDING_ACKS][12];
static int pending_n = 0;

static void savePendingAcks() {
    if (!fs_ok) return;
    waitEPDReady();
    if (pending_n == 0) { LittleFS.remove(PENDING_ACK_FILE); return; }
    File f = LittleFS.open(PENDING_ACK_FILE, FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < pending_n; i++) f.printf("%s %s\n", pending_ack[i], pending_res[i]);
    f.close();
}

void loadPendingAcks() {
    pending_n = 0;
    if (!fs_ok || !LittleFS.exists(PENDING_ACK_FILE)) return;
    File f = LittleFS.open(PENDING_ACK_FILE, FILE_READ);
    if (!f) return;
    while (f.available() && pending_n < MAX_PENDING_ACKS) {
        String l = f.readStringUntil('\n'); l.trim();
        int sp = l.indexOf(' ');
        if (sp <= 0) continue;
        strlcpy(pending_ack[pending_n], l.substring(0, sp).c_str(), 40);
        strlcpy(pending_res[pending_n], l.substring(sp + 1).c_str(), 12);
        pending_n++;
    }
    f.close();
    Serial.printf("Pending acks loaded: %d\n", pending_n);
}

bool isAlarmLocallyAcked(const char* alarm_id) {
    for (int i = 0; i < pending_n; i++) if (strcmp(pending_ack[i], alarm_id) == 0) return true;
    return false;
}

void alarmIdOf(const EventItem& e, int slot, char* out, size_t outSize) {
    snprintf(out, outSize, "%s-%+d", e.id, e.offset_min[slot]);
}

static bool postAck(const char* alarm_id, const char* result) {
    ensureBuffers();
    snprintf(body_buf, RESP_BUF_SIZE, "{\"alarm_id\":\"%s\",\"result\":\"%s\",\"fw\":\"%s\"}",
             alarm_id, result, BUILD_VERSION);
    int code = 0;
    bool ok = httpPostJson("/api/v1/alarm/ack", body_buf, resp_buf, RESP_BUF_SIZE, &code);
    Serial.printf("ACK %s -> %s (HTTP %d)\n", alarm_id, ok ? "OK" : "FAIL", code);
    return ok;
}

void ackAlarm(const char* alarm_id, const char* result) {
    if (!alarm_id || !alarm_id[0]) return;
    logLine("ack %s %s", alarm_id, result);
    if (postAck(alarm_id, result)) return;
    // 届かなければ保持 (再起動しても残る) → 再送されるまでローカルで triggered 扱い
    if (!isAlarmLocallyAcked(alarm_id) && pending_n < MAX_PENDING_ACKS) {
        strlcpy(pending_ack[pending_n], alarm_id, 40);
        strlcpy(pending_res[pending_n], result, 12);
        pending_n++;
        savePendingAcks();
    }
}

void retryPendingAcks() {
    if (pending_n == 0 || !host_online) return;
    int i = 0;
    bool changed = false;
    while (i < pending_n) {
        if (postAck(pending_ack[i], pending_res[i])) {
            for (int j = i; j < pending_n - 1; j++) {
                strcpy(pending_ack[j], pending_ack[j + 1]);
                strcpy(pending_res[j], pending_res[j + 1]);
            }
            pending_n--;
            changed = true;
        } else {
            break;   // ホストが落ちている。次回
        }
    }
    if (changed) savePendingAcks();
}

//------------------------------------------------------------------------------
// 時刻合わせ (ホスト時刻が基準。NTPは使わない)
//   ホスト同期のたびに BM8563 RTC にも書き込み、ホスト不達のまま再起動しても
//   RTCから時刻を復元できるようにする (rtcLoadTime)。RTCはUTCで保持しTZ非依存。
//------------------------------------------------------------------------------
static double max_skew_sec = 2.0;
static unsigned long last_rtc_store_ms = 0;
#define RTC_STORE_INTERVAL_MS (6UL * 3600UL * 1000UL)   // ドリフト抑制のため6時間毎に書き直す

// UTC epoch ← 年月日時分秒 (days-from-civil。mktime/timegmのTZ依存を避ける)
static time_t civilToEpochUTC(int y, int m, int d, int hh, int mm, int ss) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153U * (unsigned)(m + (m > 2 ? -3 : 9)) + 2U) / 5U + (unsigned)d - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + hh * 3600 + mm * 60 + ss;
}

static void rtcStoreTime() {
    time_t now = time(nullptr);
    struct tm g; gmtime_r(&now, &g);
    rtc_time_t t((int8_t)g.tm_hour, (int8_t)g.tm_min, (int8_t)g.tm_sec);
    rtc_date_t d((int8_t)g.tm_wday, (int8_t)(g.tm_mon + 1), (int8_t)g.tm_mday, (int16_t)(g.tm_year + 1900));
    M5.RTC.setTime(&t);     // 秒レジスタ書込みでVLフラグもクリアされる
    M5.RTC.setDate(&d);
    last_rtc_store_ms = millis();
}

bool rtcLoadTime() {
    if (M5.RTC.readReg(0x02) & 0x80) {          // VL: 電圧低下で時刻喪失
        Serial.println("RTC: VL flag set - time not trusted");
        return false;
    }
    rtc_time_t t; rtc_date_t d;
    M5.RTC.getTime(&t);
    M5.RTC.getDate(&d);
    if (d.year < 2026 || d.year > 2099 || d.mon < 1 || d.mon > 12 || d.day < 1 || d.day > 31) return false;
    struct timeval tv;
    tv.tv_sec = civilToEpochUTC(d.year, d.mon, d.day, t.hour, t.min, t.sec);
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    time_valid = true;
    Serial.printf("TIME set from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  d.year, d.mon, d.day, t.hour, t.min, t.sec);
    return true;
}

void applyTimeFromHost(double hostNow, unsigned long rttMs) {
    if (hostNow < 1600000000.0) return;
    double est = hostNow + (rttMs / 2000.0);
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    double local = tv.tv_sec + tv.tv_usec / 1e6;
    double skew = local - est;
    bool first = !time_valid;
    if (first || fabs(skew) > max_skew_sec) {
        tv.tv_sec = (time_t)est;
        tv.tv_usec = (suseconds_t)((est - (double)tv.tv_sec) * 1e6);
        settimeofday(&tv, nullptr);
        time_valid = true;
        rtcStoreTime();
        Serial.printf("TIME %s from host: skew %+.2fs rtt %lums\n", first ? "set" : "corrected", skew, rttMs);
        if (!first) logLine("time corrected skew=%+.2fs", skew);
    } else if (millis() - last_rtc_store_ms > RTC_STORE_INTERVAL_MS) {
        rtcStoreTime();
    }
}

//------------------------------------------------------------------------------
// ヘッダ行の適用 (表示設定はホストが決める)
//------------------------------------------------------------------------------
static void applyHeader(JsonObject h) {
    if (h.containsKey("time_24h"))   config.time_24h = h["time_24h"];
    if (h.containsKey("text_wrap"))  config.text_wrap = h["text_wrap"];
    if (h.containsKey("play_duration")) config.play_duration = h["play_duration"];
    if (h.containsKey("play_repeat"))   config.play_repeat = h["play_repeat"];
    if (h.containsKey("alarm_offset"))  config.alarm_offset_default = h["alarm_offset"];
    if (h["midi_default"]) strlcpy(config.midi_default, h["midi_default"], sizeof(config.midi_default));
    if (h["tz"]) {
        const char* tz = h["tz"];
        if (strcmp(tz, config.tz) != 0) {
            strlcpy(config.tz, tz, sizeof(config.tz));
            setenv("TZ", config.tz, 1); tzset();
        }
    }
    src_fail_mask = 0;
    if (h["src_fail"].is<JsonArray>()) {
        for (int v : h["src_fail"].as<JsonArray>()) if (v >= 1 && v <= 8) src_fail_mask |= (1 << (v - 1));
    }
}

//------------------------------------------------------------------------------
// 1行 → EventItem
//------------------------------------------------------------------------------
static bool fillEvent(JsonObject o, EventItem& e) {
    const char* id = o["id"];
    if (!id || !id[0]) return false;
    memset(&e, 0, sizeof(EventItem));
    strlcpy(e.id, id, sizeof(e.id));
    e.start = (time_t)(o["st"] | 0L);
    e.is_allday = (o["ad"] | 0) != 0;
    const char* s = o["s"] | "";
    const char* d = o["d"] | "";
    size_t bufSize = sizeof(e.text);
    size_t sumLen = strlen(s);
    if (sumLen >= bufSize - 2) sumLen = bufSize - 2;
    memcpy(e.text, s, sumLen);
    e.text[sumLen] = '\0';
    size_t descPos = sumLen + 1;
    size_t descSpace = bufSize - descPos - 1;
    size_t descLen = strlen(d);
    if (descLen > descSpace) {
        descLen = descSpace;
        while (descLen > 0 && ((uint8_t)d[descLen] & 0xC0) == 0x80) descLen--;
    }
    memcpy(e.text + descPos, d, descLen);
    e.text[descPos + descLen] = '\0';

    strlcpy(e.midi_file, o["mf"] | "", sizeof(e.midi_file));
    e.midi_is_url = (o["mu"] | 0) != 0;
    e.play_duration_sec = o["pd"] | -1;
    e.play_repeat = o["pr"] | -1;
    e.alarm_count = 0;
    JsonArray al = o["al"].as<JsonArray>();
    if (!al.isNull()) {
        for (JsonObject a : al) {
            if (e.alarm_count >= MAX_ALARMS_PER_EVENT) break;
            int k = e.alarm_count;
            e.offset_min[k] = a["off"] | 0;
            e.alarm_time[k] = (time_t)(a["at"] | 0L);
            e.triggered[k] = (a["tr"] | 0) != 0;
            if (!e.triggered[k]) {
                char aid[40];
                alarmIdOf(e, k, aid, sizeof(aid));
                if (isAlarmLocallyAcked(aid)) e.triggered[k] = true;   // 端末では鳴動済み(ACK未達)
            }
            e.alarm_count++;
        }
    }
    e.has_alarm = e.alarm_count > 0;
    return true;
}

//------------------------------------------------------------------------------
// 全件同期
//------------------------------------------------------------------------------
bool fetchAndUpdate() {
    ensureBuffers();
    if (!line_buf || !doc) { Serial.println("SYNC: no PSRAM buffers"); return false; }
    if (WiFi.status() != WL_CONNECTED) { sync_fail_count++; return false; }

    unsigned long t0 = millis();
    WiFiClient c;
    HttpResult r;
    if (!httpGetBegin(c, "/api/v1/events", r) || r.code != 200) {
        Serial.printf("SYNC: GET /api/v1/events failed (HTTP %d)\n", r.code);
        c.stop();
        sync_fail_count++;
        return false;
    }

    EventItem* dst = (events == events_buf_a) ? events_buf_b : events_buf_a;
    int n = 0;
    long rev = -1;
    bool gotHeader = false, gotEnd = false;
    int bad = 0;
    int expected = -1;

    while (true) {
        esp_task_wdt_reset();
        int l = httpReadLine(c, line_buf, LINE_BUF_SIZE, HTTP_TIMEOUT_MS);
        if (l < 0) break;
        if (l == 0) continue;
        doc->clear();
        DeserializationError err = deserializeJson(*doc, line_buf, l);
        if (err) {
            bad++;
            Serial.printf("SYNC: line %d JSON error %s (len=%d)\n", n, err.c_str(), l);
            if (bad > 20) break;
            continue;
        }
        JsonObject o = doc->as<JsonObject>();
        const char* type = o["type"];
        if (type && strcmp(type, "header") == 0) {
            gotHeader = true;
            rev = o["rev"] | -1L;
            expected = o["count"] | -1;
            applyHeader(o);
            if (o["now"].is<double>()) applyTimeFromHost(o["now"].as<double>(), millis() - t0);
            continue;
        }
        if (type && strcmp(type, "end") == 0) { gotEnd = true; break; }
        if (n >= MAX_EVENTS) continue;
        if (fillEvent(o, dst[n])) n++;
    }
    c.stop();

    if (!gotHeader || !gotEnd || (expected >= 0 && n != min(expected, MAX_EVENTS))) {
        Serial.printf("SYNC: incomplete (header:%d end:%d n=%d expected=%d) - keeping old table\n",
                      gotHeader, gotEnd, n, expected);
        logLine("sync incomplete n=%d exp=%d", n, expected);
        sync_fail_count++;
        return false;
    }

    // 置換 (ダブルバッファ: 解析中も旧テーブルで描画/アラーム判定できる)
    int before = event_count;
    events = dst;
    event_count = n;
    local_rev = rev;
    host_rev = rev;
    last_fetch = time(nullptr);
    sync_fail_count = 0;
    if (selected_event >= event_count) selected_event = max(0, event_count - 1);
    if (page_start >= event_count) page_start = max(0, event_count - 1);

    int pending = 0;
    for (int i = 0; i < event_count; i++)
        for (int k = 0; k < events[i].alarm_count; k++) if (!events[i].triggered[k]) pending++;
    Serial.printf("SYNC: %d -> %d events, rev=%ld, pending alarms=%d, %lums, heap:%u\n",
                  before, n, rev, pending, millis() - t0, ESP.getFreeHeap());
    logLine("sync ok ev=%d rev=%ld pend=%d %lums", n, rev, pending, millis() - t0);
    saveEventsCache();      // ホスト不達での再起動に備えて永続化 (rev不変ならスキップ)
    return true;
}

//------------------------------------------------------------------------------
// コマンド処理 (ホスト → 端末)
//------------------------------------------------------------------------------
static long last_cmd_id = 0;

static void postCmdAck(long id, bool ok, const char* info) {
    ensureBuffers();
    snprintf(body_buf, RESP_BUF_SIZE, "{\"id\":%ld,\"ok\":%s,\"info\":\"%s\"}", id, ok ? "true" : "false", info);
    httpPostJson("/api/v1/cmd/ack", body_buf, resp_buf, RESP_BUF_SIZE);
}

static void handleCommand(JsonObject c) {
    long id = c["id"] | 0L;
    const char* cmd = c["cmd"] | "";
    if (id == 0 || !cmd[0]) return;
    if (id <= last_cmd_id) { postCmdAck(id, true, "dup"); return; }   // 再送分
    last_cmd_id = id;
    Serial.printf("CMD #%ld %s\n", id, cmd);
    logLine("cmd #%ld %s", id, cmd);
    uiEventPush("cmd", 0, 0, cmd);
    bool ok = true;
    char info[48] = "";

    if (strcmp(cmd, "ping") == 0) {
        snprintf(info, sizeof(info), "pong heap=%u", ESP.getFreeHeap());
    } else if (strcmp(cmd, "refresh") == 0) {
        ok = fetchAndUpdate();
        if (ui_state == UI_LIST) { scrollToToday(); partial_refresh_count = 0; drawList(); }
    } else if (strcmp(cmd, "redraw") == 0) {
        if (ui_state == UI_LIST) { partial_refresh_count = 0; drawList(); }
        else if (ui_state == UI_DETAIL) drawDetail(selected_event);
    } else if (strcmp(cmd, "screenshot") == 0) {
        ok = uploadScreenshot();
    } else if (strcmp(cmd, "reboot") == 0) {
        logLine("reboot by host command");
        postCmdAck(id, true, "rebooting");
        delay(200);
        ESP.restart();
        return;
    } else if (strcmp(cmd, "stop") == 0) {
        if (midi_playing) finishAlarm();
        else ok = false, strcpy(info, "not playing");
    } else if (strcmp(cmd, "play") == 0) {
        const char* midiName = c["midi"] | "";
        bool isUrl = c["midi_is_url"] | true;
        int dur = c["duration"] | -1;
        int rep = c["repeat"] | -1;
        const char* aid = c["alarm_id"] | "";
        const char* eid = c["event_id"] | "";
        if (midi_playing) { ok = false; strcpy(info, "busy"); }
        else ok = startCommandPlay(midiName, isUrl, dur, rep, aid, eid);
    } else if (strcmp(cmd, "message") == 0) {
        const char* text = c["text"] | "";
        int hold = c["hold_ms"] | 8000;
        showMessage(text, hold);
    } else if (strcmp(cmd, "show") == 0) {
        const char* eid = c["event_id"] | "";
        int idx = findEventById(eid);
        if (idx >= 0) { selected_event = idx; ui_state = UI_DETAIL; detail_scroll = 0; drawDetail(idx); }
        else { ok = false; strcpy(info, "no such event"); }
    } else if (strcmp(cmd, "config") == 0) {
        if (c.containsKey("time_24h"))  config.time_24h = c["time_24h"];
        if (c.containsKey("text_wrap")) config.text_wrap = c["text_wrap"];
        if (ui_state == UI_LIST) drawList();
    } else {
        ok = false; strcpy(info, "unknown");
    }
    postCmdAck(id, ok, info);
}

//------------------------------------------------------------------------------
// ハートビート (Active Sensing)
//------------------------------------------------------------------------------
static uint32_t hb_seq = 0;

static const char* uiName() {
    switch (ui_state) {
        case UI_LIST: return "list"; case UI_DETAIL: return "detail"; case UI_PLAYING: return "playing";
        case UI_SETTINGS: return "settings"; case UI_KEYBOARD: return "keyboard";
        case UI_MIDI_SELECT: return "midisel"; case UI_BAUD_SELECT: return "baudsel"; case UI_PORT_SELECT: return "portsel";
    }
    return "?";
}

static void jsonEscapeInto(char* dst, size_t dstSize, const char* src) {
    size_t j = 0;
    for (; *src && j < dstSize - 2; src++) {
        unsigned char ch = (unsigned char)*src;
        if (ch == '"' || ch == '\\') { if (j + 2 >= dstSize - 1) break; dst[j++] = '\\'; dst[j++] = ch; }
        else if (ch < 0x20) { dst[j++] = ' '; }
        else dst[j++] = ch;
    }
    dst[j] = '\0';
}

bool sendHeartbeat(bool force) {
    static unsigned long last_hb_ms = 0;
    unsigned long interval = midi_playing ? HB_INTERVAL_PLAYING_MS : hb_interval_ms;
    if (!force && millis() - last_hb_ms < interval) return host_online;
    last_hb_ms = millis();
    ensureBuffers();
    if (WiFi.status() != WL_CONNECTED) {
        host_online = false;
        if (host_lost_since_ms == 0) host_lost_since_ms = millis();
        return false;
    }

    esp_reset_reason_t reason = esp_reset_reason();
    int pending = 0;
    for (int i = 0; i < event_count; i++)
        for (int k = 0; k < events[i].alarm_count; k++) if (!events[i].triggered[k]) pending++;
    struct timeval tv; gettimeofday(&tv, nullptr);
    float tC = -99.0f;
    if (M5.SHT30.UpdateData() == 0) tC = M5.SHT30.GetTemperature();

    int n = snprintf(body_buf, RESP_BUF_SIZE,
        "{\"fw\":\"%s\",\"seq\":%u,\"uptime\":%lu,\"now\":%ld.%03ld,\"heap\":%u,\"maxblock\":%u,\"psram\":%u,"
        "\"bat\":%u,\"rssi\":%d,\"bssid\":\"%s\",\"ch\":%d,\"ip\":\"%s\",\"ui\":\"%s\",\"rev\":%ld,\"reset\":%d,\"events\":%d,\"pending\":%d,"
        "\"sync_fail\":%d,\"fs_used\":%u,\"fs_total\":%u,\"temp\":%.1f,\"playing\":%s%s%s,\"pend_ack\":%d",
        BUILD_VERSION, (unsigned)hb_seq++, millis() / 1000UL, (long)tv.tv_sec, (long)(tv.tv_usec / 1000),
        ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram(),
        (unsigned)M5.getBatteryVoltage(), WiFi.RSSI(), WiFi.BSSIDstr().c_str(), WiFi.channel(),
        WiFi.localIP().toString().c_str(), uiName(),
        local_rev, (int)reason, event_count, pending, sync_fail_count, (unsigned)fsUsed(), (unsigned)fsTotal(), tC,
        midi_playing ? "\"" : "null", midi_playing ? (playing_alarm_id[0] ? playing_alarm_id : "test") : "",
        midi_playing ? "\"" : "", pending_n);

    // 操作イベント
    UiEvent evs[UI_EVENT_QUEUE];
    int ne = uiEventDrain(evs, UI_EVENT_QUEUE);
    n += snprintf(body_buf + n, RESP_BUF_SIZE - n, ",\"ev\":[");
    for (int i = 0; i < ne && n < (int)RESP_BUF_SIZE - 200; i++) {
        char esc[48]; jsonEscapeInto(esc, sizeof(esc), evs[i].info);
        n += snprintf(body_buf + n, RESP_BUF_SIZE - n, "%s{\"ms\":%u,\"k\":\"%s\",\"x\":%d,\"y\":%d,\"i\":\"%s\"}",
                      i ? "," : "", (unsigned)evs[i].ms, evs[i].kind, evs[i].x, evs[i].y, esc);
    }
    n += snprintf(body_buf + n, RESP_BUF_SIZE - n, "]");
    // ログ
    char* logs[LOG_QUEUE];
    int nl = logDrain(logs, LOG_QUEUE);
    n += snprintf(body_buf + n, RESP_BUF_SIZE - n, ",\"log\":[");
    for (int i = 0; i < nl && n < (int)RESP_BUF_SIZE - 400; i++) {
        char esc[LOG_LINE_LEN * 2]; jsonEscapeInto(esc, sizeof(esc), logs[i]);
        n += snprintf(body_buf + n, RESP_BUF_SIZE - n, "%s\"%s\"", i ? "," : "", esc);
    }
    n += snprintf(body_buf + n, RESP_BUF_SIZE - n, "]}");

    unsigned long t0 = millis();
    int code = 0;
    bool ok = httpPostJson("/api/v1/heartbeat", body_buf, resp_buf, RESP_BUF_SIZE, &code);
    unsigned long rtt = millis() - t0;
    if (!ok) {
        if (host_online) {
            Serial.printf("HB: host unreachable (HTTP %d)\n", code);
            // 切断原因の切り分け用: その瞬間の電波強度・WiFi状態・チャンネルを残す
            logLine("host lost code=%d rssi=%d st=%d ch=%d", code, WiFi.RSSI(), (int)WiFi.status(), WiFi.channel());
        }
        host_online = false;
        if (host_lost_since_ms == 0) host_lost_since_ms = millis();
        return false;
    }
    doc->clear();
    DeserializationError err = deserializeJson(*doc, resp_buf);
    if (err) { Serial.printf("HB: bad JSON %s\n", err.c_str()); host_online = false; return false; }
    JsonObject o = doc->as<JsonObject>();
    if (!host_online) { Serial.printf("HB: host online (rtt %lums)\n", rtt); logLine("host online rtt=%lu", rtt); }
    host_online = true;
    host_lost_since_ms = 0;

    if (o["now"].is<double>()) applyTimeFromHost(o["now"].as<double>(), rtt);
    if (o.containsKey("max_skew")) max_skew_sec = o["max_skew"].as<double>();
    if (o.containsKey("hb_sec")) { int s = o["hb_sec"]; if (s >= 2 && s <= 120) hb_interval_ms = s * 1000UL; }
    if (o.containsKey("full_sync_sec")) { int s = o["full_sync_sec"]; if (s >= 30) full_sync_sec = s; }
    if (o["tz"]) { const char* tz = o["tz"]; if (strcmp(tz, config.tz) != 0) { strlcpy(config.tz, tz, sizeof(config.tz)); setenv("TZ", config.tz, 1); tzset(); } }
    host_rev = o["rev"] | host_rev;
    host_next_alarm = (time_t)(o["next_alarm"] | 0L);

    JsonArray cmds = o["cmds"].as<JsonArray>();
    if (!cmds.isNull()) {
        // コマンドは doc を再利用する処理(refresh等)を含むためコピーしてから実行
        StaticJsonDocument<2048> copy;
        copy.set(cmds);
        for (JsonObject c : copy.as<JsonArray>()) handleCommand(c);
    }
    return true;
}

//==============================================================================
// 安全リブート（MIDI再生中は延期）
//==============================================================================
void safeReboot() {
    if (midi_playing) {
        Serial.println("REBOOT DEFERRED: MIDI playing");
        reboot_pending = true;
        return;
    }
    Serial.println("=== reboot ===");
    sendHeartbeat(true);
    Serial.flush();
    delay(100);
    ESP.restart();
}
