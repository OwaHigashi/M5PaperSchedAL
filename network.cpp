#include "globals.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>

bool connectWiFi() {
    if (strlen(config.wifi_ssid) == 0) return false;

    WiFi.disconnect(true);
    delay(100);

    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifi_ssid, config.wifi_pass);

    Serial.print("Connecting to WiFi");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected: " + WiFi.localIP().toString());
        Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
        return true;
    }
    Serial.println("WiFi connection failed");
    return false;
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

    String url = "https://ntfy.sh/" + String(config.ntfy_topic);
    Serial.printf("NTFY: Sending to %s\n", url.c_str());

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);

    if (!http.begin(client, url)) {
        Serial.println("NTFY: HTTP begin failed");
        return;
    }

    http.addHeader("Title", title);
    http.addHeader("Priority", "high");
    http.addHeader("Tags", "alarm_clock");

    int code = http.POST(message);
    if (code > 0) {
        Serial.printf("NTFY: HTTP %d\n", code);
    } else {
        Serial.printf("NTFY: HTTP error %d\n", code);
    }
    http.end();
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

    String url = String(config.midi_url);
    if (!url.endsWith("/")) url += "/";
    url += filename;

    Serial.printf("Downloading MIDI: %s\n", url.c_str());

    HTTPClient http;
    http.begin(url);
    int code = http.GET();

    if (code != HTTP_CODE_OK) {
        Serial.printf("MIDI download failed: %d\n", code);
        http.end();
        return false;
    }

    File f = SD.open(localPath.c_str(), FILE_WRITE);
    if (!f) {
        Serial.println("Failed to create MIDI file");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[512];
    while (http.connected() && stream->available()) {
        int len = stream->read(buf, sizeof(buf));
        if (len > 0) f.write(buf, len);
    }
    f.flush();
    f.close();
    http.end();

    Serial.printf("MIDI downloaded: %s\n", localPath.c_str());
    return true;
}
