#include "globals.h"
#include <esp_task_wdt.h>

//==============================================================================
// EPD描画完了待ち
//   pushCanvas()は非同期で戻ることがある。IT8951が無応答だとCheckAFSR()が
//   ブロックするため Task WDT で回復する (setup参照)。
//==============================================================================
void waitEPDReady() {
    M5.EPD.CheckAFSR();
}

//==============================================================================
// LittleFS (内蔵フラッシュ) 初期化。SDカードは使用しない。
//   data/ 以下を `pio run -t uploadfs` で書き込む: /config.json /fonts/ipaexg.ttf /midi/*.mid
//==============================================================================
bool initFS() {
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed - formatting");
        if (!LittleFS.begin(true)) {
            Serial.println("LittleFS format failed");
            fs_ok = false;
            return false;
        }
    }
    fs_ok = true;
    if (!LittleFS.exists(MIDI_DL_DIR)) LittleFS.mkdir(MIDI_DL_DIR);
    Serial.printf("LittleFS: %u / %u bytes used\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    return true;
}

size_t fsUsed()  { return fs_ok ? LittleFS.usedBytes() : 0; }
size_t fsTotal() { return fs_ok ? LittleFS.totalBytes() : 0; }

//==============================================================================
// MIDIファイルスキャン (/midi と /midi-dl)
//==============================================================================
static void scanDir(const char* dirPath) {
    File dir = LittleFS.open(dirPath);
    if (!dir || !dir.isDirectory()) return;
    while (midi_file_count < 32) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String name = entry.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!entry.isDirectory() &&
            (name.endsWith(".mid") || name.endsWith(".MID") ||
             name.endsWith(".midi") || name.endsWith(".MIDI"))) {
            midi_files[midi_file_count++] = String(dirPath) + "/" + name;
        }
        entry.close();
    }
    dir.close();
}

void scanMidiFiles() {
    midi_file_count = 0;
    if (!fs_ok) return;
    scanDir(MIDI_DIR);
    scanDir(MIDI_DL_DIR);
    Serial.printf("Found %d MIDI files\n", midi_file_count);
}

//==============================================================================
// 予定テーブルのローカルキャッシュ (/events.cache)
//   ホスト不達のまま再起動しても、RTC復元時刻(rtcLoadTime)と合わせて
//   表示・鳴動を継続するための永続化。text は実使用分のみ書き出す。
//==============================================================================
#define EVENTS_CACHE_FILE   "/events.cache"
#define EVENTS_CACHE_TMP    "/events.cache.tmp"
#define EVENTS_CACHE_MAGIC  0x4345354DUL    // "M5EC"
#define EVENTS_CACHE_VER    1

static long cache_saved_rev = LONG_MIN;

static bool wrN(File& f, const void* p, size_t n) { return f.write((const uint8_t*)p, n) == n; }
static bool rdN(File& f, void* p, size_t n)       { return f.read((uint8_t*)p, n) == n; }

bool saveEventsCache() {
    if (!fs_ok) return false;
    if (local_rev == cache_saved_rev && !events_cache_dirty) return true;   // 変化なし
    unsigned long t0 = millis();
    File f = LittleFS.open(EVENTS_CACHE_TMP, FILE_WRITE);
    if (!f) { Serial.println("CACHE: open failed"); return false; }
    uint32_t magic = EVENTS_CACHE_MAGIC;
    uint16_t ver = EVENTS_CACHE_VER, cnt = (uint16_t)event_count;
    int32_t rev32 = (int32_t)local_rev;
    int64_t saved_at = (int64_t)time(nullptr);
    bool ok = wrN(f, &magic, 4) && wrN(f, &ver, 2) && wrN(f, &cnt, 2) &&
              wrN(f, &rev32, 4) && wrN(f, &saved_at, 8);
    for (int i = 0; ok && i < event_count; i++) {
        esp_task_wdt_reset();
        EventItem& e = events[i];
        int64_t start = (int64_t)e.start;
        uint8_t flags = (e.midi_is_url ? 1 : 0) | (e.has_alarm ? 2 : 0) | (e.is_allday ? 4 : 0);
        int32_t dur = e.play_duration_sec, rep = e.play_repeat;
        uint8_t ac = (uint8_t)e.alarm_count;
        ok = wrN(f, e.id, EVENT_ID_LEN) && wrN(f, &start, 8) &&
             wrN(f, e.midi_file, sizeof(e.midi_file)) && wrN(f, &flags, 1) &&
             wrN(f, &dur, 4) && wrN(f, &rep, 4) && wrN(f, &ac, 1);
        for (int k = 0; ok && k < e.alarm_count; k++) {
            int32_t off = e.offset_min[k];
            int64_t at = (int64_t)e.alarm_time[k];
            uint8_t tr = e.triggered[k] ? 1 : 0;
            ok = wrN(f, &off, 4) && wrN(f, &at, 8) && wrN(f, &tr, 1);
        }
        if (ok) {
            size_t sl = strlen(e.text);
            size_t used = sl + 1 + strlen(e.text + sl + 1) + 1;   // summary\0desc\0
            uint16_t tl = (uint16_t)min(used, sizeof(e.text));
            ok = wrN(f, &tl, 2) && wrN(f, e.text, tl);
        }
    }
    f.close();
    if (!ok) { LittleFS.remove(EVENTS_CACHE_TMP); Serial.println("CACHE: save failed"); return false; }
    LittleFS.remove(EVENTS_CACHE_FILE);
    if (!LittleFS.rename(EVENTS_CACHE_TMP, EVENTS_CACHE_FILE)) {
        Serial.println("CACHE: rename failed");
        return false;
    }
    cache_saved_rev = local_rev;
    events_cache_dirty = false;
    Serial.printf("CACHE: saved %d events rev=%ld (%lums)\n", event_count, local_rev, millis() - t0);
    return true;
}

int loadEventsCache() {
    if (!fs_ok || !LittleFS.exists(EVENTS_CACHE_FILE)) return -1;
    File f = LittleFS.open(EVENTS_CACHE_FILE, FILE_READ);
    if (!f) return -1;
    uint32_t magic = 0; uint16_t ver = 0, cnt = 0; int32_t rev32 = 0; int64_t saved_at = 0;
    bool ok = rdN(f, &magic, 4) && rdN(f, &ver, 2) && rdN(f, &cnt, 2) &&
              rdN(f, &rev32, 4) && rdN(f, &saved_at, 8) &&
              magic == EVENTS_CACHE_MAGIC && ver == EVENTS_CACHE_VER && cnt <= MAX_EVENTS;
    EventItem* dst = (events == events_buf_a) ? events_buf_b : events_buf_a;
    int n = 0;
    while (ok && n < cnt) {
        esp_task_wdt_reset();
        EventItem& e = dst[n];
        memset(&e, 0, sizeof(EventItem));
        int64_t start = 0; uint8_t flags = 0; int32_t dur = 0, rep = 0; uint8_t ac = 0;
        ok = rdN(f, e.id, EVENT_ID_LEN) && rdN(f, &start, 8) &&
             rdN(f, e.midi_file, sizeof(e.midi_file)) && rdN(f, &flags, 1) &&
             rdN(f, &dur, 4) && rdN(f, &rep, 4) && rdN(f, &ac, 1) &&
             ac <= MAX_ALARMS_PER_EVENT;
        if (!ok) break;
        e.id[EVENT_ID_LEN - 1] = '\0';
        e.midi_file[sizeof(e.midi_file) - 1] = '\0';
        e.start = (time_t)start;
        e.midi_is_url = flags & 1; e.has_alarm = flags & 2; e.is_allday = flags & 4;
        e.play_duration_sec = dur; e.play_repeat = rep;
        e.alarm_count = ac;
        for (int k = 0; ok && k < ac; k++) {
            int32_t off = 0; int64_t at = 0; uint8_t tr = 0;
            ok = rdN(f, &off, 4) && rdN(f, &at, 8) && rdN(f, &tr, 1);
            e.offset_min[k] = off; e.alarm_time[k] = (time_t)at; e.triggered[k] = tr != 0;
        }
        uint16_t tl = 0;
        ok = ok && rdN(f, &tl, 2) && tl >= 2 && tl <= sizeof(e.text) - 1 && rdN(f, e.text, tl);
        if (!ok) break;
        // 未達ACK分は鳴動済み扱い (fetchAndUpdate と同じ)
        for (int k = 0; k < e.alarm_count; k++) {
            if (e.triggered[k]) continue;
            char aid[40]; alarmIdOf(e, k, aid, sizeof(aid));
            if (isAlarmLocallyAcked(aid)) e.triggered[k] = true;
        }
        n++;
    }
    f.close();
    if (!ok || n != cnt) {
        Serial.printf("CACHE: corrupt (n=%d/%u) - ignored\n", n, cnt);
        return -1;
    }
    events = dst;
    event_count = n;
    local_rev = rev32;
    cache_saved_rev = rev32;
    events_cache_dirty = false;
    long age = (long)(time(nullptr) - (time_t)saved_at);
    Serial.printf("CACHE: loaded %d events rev=%ld age=%lds\n", n, (long)rev32, age);
    logLine("cache load ev=%d rev=%ld age=%lds", n, (long)rev32, age);
    return n;
}
