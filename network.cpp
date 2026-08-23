#include "globals.h"
#include <esp_task_wdt.h>

//==============================================================================
// WiFi
//==============================================================================
bool connectWiFi() {
    if (strlen(config.wifi_ssid) == 0) return false;

    for (int attempt = 1; attempt <= 3; attempt++) {
        WiFi.disconnect(true);
        if (attempt >= 2) {
            WiFi.mode(WIFI_OFF);
            delay(300);
            Serial.printf("WiFi radio full reset (attempt %d)\n", attempt);
        } else {
            delay(100);
        }
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);          // レイテンシ優先 (5秒毎のハートビートが安定する)
        WiFi.setAutoReconnect(true);
        WiFi.begin(config.wifi_ssid, config.wifi_pass);

        Serial.printf("Connecting to WiFi '%s' (attempt %d/3)", config.wifi_ssid, attempt);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
            delay(250);
            Serial.print(".");
            esp_task_wdt_reset();
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("WiFi connected: %s RSSI:%d (heap: %d)\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap());
            return true;
        }
        Serial.printf("WiFi attempt %d failed (status: %d)\n", attempt, WiFi.status());
    }
    Serial.println("WiFi connection failed after 3 attempts");
    return false;
}

//==============================================================================
// 素のHTTP/1.1 クライアント (WiFiClient直接、String/HTTPClient不使用、SSLなし)
//   ホストは常に Content-Length を付け chunked を使わない (server/m5sched/api.py)。
//==============================================================================
int httpReadLine(WiFiClient& c, char* buf, int maxLen, unsigned long timeoutMs) {
    int i = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (!c.available()) {
            if (!c.connected()) return i > 0 ? i : -1;
            delay(1);
            continue;
        }
        int b = c.read();
        if (b < 0) continue;
        if (b == '\n') { buf[i] = '\0'; return i; }
        if (b != '\r' && i < maxLen - 1) buf[i++] = (char)b;
    }
    buf[i] = '\0';
    return i > 0 ? i : -1;
}

bool httpBegin(WiFiClient& c, const char* method, const char* path,
               const char* contentType, size_t bodyLen, HttpResult& r,
               const char* extraHeaders) {
    r.code = 0; r.length = -1;
    if (WiFi.status() != WL_CONNECTED) return false;
    c.setTimeout(HTTP_TIMEOUT_MS / 1000);
    if (!c.connect(config.server_host, config.server_port, HTTP_TIMEOUT_MS)) {
        return false;
    }
    c.setNoDelay(true);
    char req[640];
    int n = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: M5PaperSched/%s\r\n"
        "Connection: close\r\n"
        "%s%s%s",
        method, path, config.server_host, config.server_port, BUILD_VERSION,
        strlen(config.api_token) ? "X-Token: " : "",
        strlen(config.api_token) ? config.api_token : "",
        strlen(config.api_token) ? "\r\n" : "");
    if (contentType) {
        n += snprintf(req + n, sizeof(req) - n, "Content-Type: %s\r\nContent-Length: %u\r\n",
                      contentType, (unsigned)bodyLen);
    }
    if (extraHeaders) n += snprintf(req + n, sizeof(req) - n, "%s", extraHeaders);
    n += snprintf(req + n, sizeof(req) - n, "\r\n");
    c.write((const uint8_t*)req, n);
    return true;
}

// ステータス行とヘッダを読み、Content-Length を r.length に入れる
static bool httpReadHeaders(WiFiClient& c, HttpResult& r) {
    char line[256];
    int l = httpReadLine(c, line, sizeof(line), HTTP_TIMEOUT_MS);
    if (l <= 0) return false;
    const char* sp = strchr(line, ' ');
    r.code = sp ? atoi(sp + 1) : 0;
    while (true) {
        l = httpReadLine(c, line, sizeof(line), HTTP_TIMEOUT_MS);
        if (l < 0) return false;
        if (l == 0) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) r.length = atol(line + 15);
    }
    return r.code > 0;
}

long httpReadBody(WiFiClient& c, char* buf, long maxLen, long contentLen, unsigned long timeoutMs) {
    long got = 0;
    unsigned long t0 = millis();
    long want = (contentLen >= 0) ? contentLen : maxLen - 1;
    if (want > maxLen - 1) want = maxLen - 1;
    while (got < want && millis() - t0 < timeoutMs) {
        int avail = c.available();
        if (avail <= 0) {
            if (!c.connected()) break;
            delay(1);
            continue;
        }
        int n = c.read((uint8_t*)buf + got, (size_t)min<long>(avail, want - got));
        if (n > 0) { got += n; t0 = millis(); }
    }
    buf[got] = '\0';
    return got;
}

// JSON POST → 応答本文を respBuf に。戻り値: HTTP 2xx かつ本文取得成功
bool httpPostJson(const char* path, const char* body, char* respBuf, size_t respSize, int* codeOut) {
    WiFiClient c;
    HttpResult r;
    if (!httpBegin(c, "POST", path, "application/json", strlen(body), r)) return false;
    c.write((const uint8_t*)body, strlen(body));
    if (!httpReadHeaders(c, r)) { c.stop(); return false; }
    httpReadBody(c, respBuf, respSize, r.length, HTTP_TIMEOUT_MS);
    c.stop();
    if (codeOut) *codeOut = r.code;
    return r.code >= 200 && r.code < 300;
}

// GET → ヘッダ読み取りまで。本文はストリームから呼び出し側が読む
bool httpGetBegin(WiFiClient& c, const char* path, HttpResult& r) {
    if (!httpBegin(c, "GET", path, nullptr, 0, r)) return false;
    if (!httpReadHeaders(c, r)) { c.stop(); return false; }
    return true;
}

//==============================================================================
// MIDI取得: ホストの /api/v1/midi/<name> から (ホストが外部URLからDL・キャッシュ済)
//==============================================================================
bool downloadMidi(const String& filename, String& localPath) {
    if (!fs_ok) return false;
    localPath = String(MIDI_DL_DIR) + "/" + filename;
    if (LittleFS.exists(localPath.c_str())) {
        return true;
    }
    char path[160];
    snprintf(path, sizeof(path), "/api/v1/midi/%s", filename.c_str());
    WiFiClient c;
    HttpResult r;
    if (!httpGetBegin(c, path, r) || r.code != 200) {
        Serial.printf("MIDI download failed: %s HTTP %d\n", filename.c_str(), r.code);
        c.stop();
        return false;
    }
    waitEPDReady();
    File f = LittleFS.open(localPath.c_str(), FILE_WRITE);
    if (!f) { c.stop(); return false; }
    uint8_t buf[1024];
    long got = 0;
    unsigned long t0 = millis();
    while ((r.length < 0 || got < r.length) && millis() - t0 < 30000) {
        esp_task_wdt_reset();
        int avail = c.available();
        if (avail <= 0) { if (!c.connected()) break; delay(1); continue; }
        int n = c.read(buf, min<int>(avail, sizeof(buf)));
        if (n > 0) { f.write(buf, n); got += n; t0 = millis(); }
    }
    f.close();
    c.stop();
    if (r.length >= 0 && got != r.length) {
        Serial.printf("MIDI download incomplete (%ld/%ld)\n", got, r.length);
        LittleFS.remove(localPath.c_str());
        return false;
    }
    Serial.printf("MIDI downloaded: %s (%ld bytes)\n", localPath.c_str(), got);
    logLine("midi dl %s %ld", filename.c_str(), got);
    return true;
}

//==============================================================================
// スクリーンショット: フレームバッファ(4bpp)をそのままホストへ POST
//==============================================================================
bool uploadScreenshot() {
    waitEPDReady();
    uint8_t* buffer = (uint8_t*)canvas.frameBuffer(1);
    uint32_t size = canvas.getBufferSize();
    if (!buffer || size == 0) return false;
    char extra[64];
    snprintf(extra, sizeof(extra), "X-Width: %d\r\nX-Height: %d\r\n", canvas.width(), canvas.height());
    WiFiClient c;
    HttpResult r;
    if (!httpBegin(c, "POST", "/api/v1/screenshot", "application/octet-stream", size, r, extra)) return false;
    uint32_t sent = 0;
    while (sent < size) {
        esp_task_wdt_reset();
        size_t n = c.write(buffer + sent, min<uint32_t>(4096, size - sent));
        if (n == 0) { if (!c.connected()) break; delay(1); continue; }
        sent += n;
    }
    bool ok = httpReadHeaders(c, r) && r.code == 200;
    c.stop();
    Serial.printf("Screenshot upload: %s (%u bytes, HTTP %d)\n", ok ? "OK" : "FAIL", sent, r.code);
    return ok;
}
