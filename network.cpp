#include "globals.h"
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <SD.h>

// ★ HTTPClient完全排除 — 全HTTP通信をWiFiClient/WiFiClientSecure直接操作
//    HTTPClient内部のString操作がSSLバッファと交互にDRAM mallocされ
//    断片化(~7KB/fetch)を起こしていた問題を根本解決

bool connectWiFi() {
    if (strlen(config.wifi_ssid) == 0) return false;

    for (int attempt = 1; attempt <= 3; attempt++) {
        WiFi.disconnect(true);
        delay(200);

        WiFi.mode(WIFI_STA);
        WiFi.begin(config.wifi_ssid, config.wifi_pass);

        Serial.printf("Connecting to WiFi (attempt %d/3)", attempt);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("WiFi connected: %s (heap: %d)\n",
                          WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
            return true;
        }
        Serial.printf("WiFi attempt %d failed (status: %d)\n", attempt, WiFi.status());
    }
    Serial.println("WiFi connection failed after 3 attempts");
    return false;
}

// HTTPレスポンスのヘッダーを1行読む (スタック上、malloc ゼロ)
static int readHttpLine(WiFiClient* c, char* buf, int maxLen, unsigned long timeoutMs = 10000) {
    int i = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
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

void sendNtfyNotification(const String& title, const String& message) {
    if (strlen(config.ntfy_topic) == 0) {
        Serial.println("NTFY: topic not configured, skipping");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("NTFY: WiFi not connected, skipping");
        return;
    }

    Serial.printf("NTFY: Sending to ntfy.sh/%s (heap:%d)\n",
                  config.ntfy_topic, ESP.getFreeHeap());

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);

    if (!client.connect("ntfy.sh", 443)) {
        Serial.println("NTFY: SSL connect failed");
        return;
    }

    // POSTリクエスト構築 (スタック上)
    char request[1024];
    int bodyLen = message.length();
    int reqLen = snprintf(request, sizeof(request),
        "POST /%s HTTP/1.1\r\n"
        "Host: ntfy.sh\r\n"
        "Title: %s\r\n"
        "Priority: high\r\n"
        "Tags: alarm_clock\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        config.ntfy_topic, title.c_str(), bodyLen);

    client.write((uint8_t*)request, reqLen);
    client.write((uint8_t*)message.c_str(), bodyLen);

    // レスポンス確認
    char statusLine[128];
    int sl = readHttpLine(&client, statusLine, sizeof(statusLine));
    int httpCode = 0;
    if (sl > 0) {
        const char* sp = strchr(statusLine, ' ');
        if (sp) httpCode = atoi(sp + 1);
    }

    client.stop();

    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("NTFY: HTTP %d (heap:%d)\n", httpCode, ESP.getFreeHeap());
    } else {
        Serial.printf("NTFY: HTTP error %d (heap:%d)\n", httpCode, ESP.getFreeHeap());
    }
}

bool downloadMidi(const String& filename, String& localPath) {
    waitEPDReady();

    if (strlen(config.midi_url) == 0) {
        Serial.println("midi_url not configured");
        return false;
    }

    if (!SD.exists(MIDI_DL_DIR)) {
        SD.mkdir(MIDI_DL_DIR);
    }

    localPath = String(MIDI_DL_DIR) + "/" + filename;

    if (SD.exists(localPath.c_str())) {
        Serial.printf("MIDI already downloaded: %s\n", localPath.c_str());
        return true;
    }

    // URL解析 (スタック上)
    char host[256];
    char path[512];
    int port = 80;
    bool use_ssl = false;

    char urlBuf[512];
    snprintf(urlBuf, sizeof(urlBuf), "%s", config.midi_url);
    // 末尾スラッシュ付与
    int ul = strlen(urlBuf);
    if (ul > 0 && urlBuf[ul - 1] != '/') { urlBuf[ul] = '/'; urlBuf[ul + 1] = '\0'; }

    // filename追加
    int remaining = sizeof(urlBuf) - strlen(urlBuf) - 1;
    strncat(urlBuf, filename.c_str(), remaining);

    const char* url = urlBuf;
    if (strncmp(url, "https://", 8) == 0) { url += 8; port = 443; use_ssl = true; }
    else if (strncmp(url, "http://", 7) == 0) { url += 7; }

    const char* pathStart = strchr(url, '/');
    if (pathStart) {
        int hostLen = pathStart - url;
        if (hostLen >= (int)sizeof(host)) hostLen = sizeof(host) - 1;
        memcpy(host, url, hostLen);
        host[hostLen] = '\0';
        snprintf(path, sizeof(path), "%s", pathStart);
    } else {
        snprintf(host, sizeof(host), "%s", url);
        strcpy(path, "/");
    }

    // ポート番号抽出
    char* colonInHost = strchr(host, ':');
    if (colonInHost) {
        port = atoi(colonInHost + 1);
        *colonInHost = '\0';
    }

    Serial.printf("Downloading MIDI: %s:%d%s\n", host, port, path);

    // 接続
    WiFiClient* client;
    WiFiClientSecure sslClient;
    WiFiClient plainClient;

    if (use_ssl) {
        sslClient.setInsecure();
        sslClient.setTimeout(10);
        if (!sslClient.connect(host, port)) {
            Serial.println("MIDI: SSL connect failed");
            return false;
        }
        client = &sslClient;
    } else {
        plainClient.setTimeout(10);
        if (!plainClient.connect(host, port)) {
            Serial.println("MIDI: connect failed");
            return false;
        }
        client = &plainClient;
    }

    // GETリクエスト
    char request[1024];
    int reqLen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    client->write((uint8_t*)request, reqLen);

    // ステータス確認
    char statusLine[128];
    int sl = readHttpLine(client, statusLine, sizeof(statusLine));
    int httpCode = 0;
    if (sl > 0) {
        const char* sp = strchr(statusLine, ' ');
        if (sp) httpCode = atoi(sp + 1);
    }

    if (httpCode != 200) {
        Serial.printf("MIDI download failed: HTTP %d\n", httpCode);
        client->stop();
        return false;
    }

    // ヘッダースキップ
    char hdr[256];
    while (true) {
        int hl = readHttpLine(client, hdr, sizeof(hdr));
        if (hl <= 0) break;
    }

    // ボディをSDに書き込み
    File f = SD.open(localPath.c_str(), FILE_WRITE);
    if (!f) {
        Serial.println("Failed to create MIDI file");
        client->stop();
        return false;
    }

    uint8_t buf[512];
    unsigned long t0 = millis();
    while (client->connected() || client->available()) {
        if (millis() - t0 > 30000) break;  // 30秒タイムアウト
        int avail = client->available();
        if (avail <= 0) { delay(1); continue; }
        int len = client->read(buf, sizeof(buf));
        if (len > 0) { f.write(buf, len); t0 = millis(); }
    }
    f.flush();
    f.close();
    client->stop();

    Serial.printf("MIDI downloaded: %s (heap:%d)\n", localPath.c_str(), ESP.getFreeHeap());
    return true;
}
