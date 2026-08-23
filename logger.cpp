#include "globals.h"
#include <stdarg.h>

//==============================================================================
// 縮約診断ログ
//   - 常に Serial へ "LOG <時刻> ..." で出力
//   - 同時にリングバッファへ積み、次のハートビートでホストへ転送 (data/log/device.log)
//   SDカードは使わない。
//==============================================================================

static char log_ring[LOG_QUEUE][LOG_LINE_LEN];
static int log_head = 0, log_count = 0;

static UiEvent ui_ring[UI_EVENT_QUEUE];
static int ui_head = 0, ui_count = 0;

void logLine(const char* fmt, ...) {
    char buf[LOG_LINE_LEN];
    int n = 0;
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        n = snprintf(buf, sizeof(buf), "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        n = snprintf(buf, sizeof(buf), "+%lus ", millis() / 1000);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    Serial.print("LOG ");
    Serial.println(buf);

    int slot = (log_head + log_count) % LOG_QUEUE;
    if (log_count == LOG_QUEUE) { log_head = (log_head + 1) % LOG_QUEUE; slot = (log_head + LOG_QUEUE - 1) % LOG_QUEUE; }
    else log_count++;
    strlcpy(log_ring[slot], buf, LOG_LINE_LEN);
}

// 未送信ログを取り出す (ポインタは次の logLine まで有効)
int logDrain(char** out, int max) {
    int n = 0;
    while (log_count > 0 && n < max) {
        out[n++] = log_ring[log_head];
        log_head = (log_head + 1) % LOG_QUEUE;
        log_count--;
    }
    return n;
}

void uiEventPush(const char* kind, int x, int y, const char* info) {
    int slot = (ui_head + ui_count) % UI_EVENT_QUEUE;
    if (ui_count == UI_EVENT_QUEUE) { ui_head = (ui_head + 1) % UI_EVENT_QUEUE; slot = (ui_head + UI_EVENT_QUEUE - 1) % UI_EVENT_QUEUE; }
    else ui_count++;
    UiEvent& e = ui_ring[slot];
    e.ms = millis();
    strlcpy(e.kind, kind, sizeof(e.kind));
    e.x = x; e.y = y;
    strlcpy(e.info, info ? info : "", sizeof(e.info));
}

int uiEventDrain(UiEvent* out, int max) {
    int n = 0;
    while (ui_count > 0 && n < max) {
        out[n++] = ui_ring[ui_head];
        ui_head = (ui_head + 1) % UI_EVENT_QUEUE;
        ui_count--;
    }
    return n;
}

//==============================================================================
// USB(シリアル)経由の簡易コマンド
//   STATUS  → 状態1行   SYNC → 全件再同期   REBOOT → 再起動   HB → ハートビート即送信
//==============================================================================
void pollSerialCommands() {
    static char line[80];
    static int li = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (li > 0) {
                line[li] = '\0';
                if (strcmp(line, "STATUS") == 0) {
                    Serial.printf("STATUS ver=%s ev=%d rev=%ld/%ld host=%s heap=%u mb=%u wifi=%d\n",
                                  BUILD_VERSION, event_count, local_rev, host_rev,
                                  host_online ? "up" : "DOWN", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.status());
                } else if (strcmp(line, "SYNC") == 0) {
                    fetchAndUpdate();
                    if (ui_state == UI_LIST) { scrollToToday(); drawList(); }
                } else if (strcmp(line, "HB") == 0) {
                    sendHeartbeat(true);
                } else if (strcmp(line, "REBOOT") == 0) {
                    ESP.restart();
                }
                li = 0;
            }
        } else if (li < (int)sizeof(line) - 1) {
            line[li++] = c;
        }
    }
}
