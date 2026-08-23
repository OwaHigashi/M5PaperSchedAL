#include "globals.h"
#include <ArduinoJson.h>

// 端末ローカル設定は LittleFS の /config.json。
// 予定・アラーム・ntfy・ICS などの設定はすべてホスト (server/config.json) 側にある。

void loadConfig() {
    strcpy(config.wifi_ssid, "your_wifi_ssid");
    strcpy(config.wifi_pass, "your_wifi_password");
    strcpy(config.server_host, DEFAULT_SERVER_HOST);
    config.server_port = DEFAULT_SERVER_PORT;
    config.api_token[0] = '\0';
    strcpy(config.midi_file, "/midi/alarm.mid");
    config.midi_baud = DEFAULT_MIDI_BAUD;
    config.port_select = 1;
    // ホストから上書きされる表示設定の初期値
    config.time_24h = true;
    config.text_wrap = false;
    config.play_duration = 0;
    config.play_repeat = 1;
    config.alarm_offset_default = 10;
    strcpy(config.midi_default, "alarm.mid");
    strcpy(config.tz, TZ_DEFAULT);

    if (!fs_ok || !LittleFS.exists(CONFIG_FILE)) {
        Serial.println("Config not found, using defaults");
        return;
    }
    File f = LittleFS.open(CONFIG_FILE, FILE_READ);
    if (!f) return;
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { Serial.printf("Config JSON error: %s\n", err.c_str()); return; }

    if (doc["wifi_ssid"])   strlcpy(config.wifi_ssid, doc["wifi_ssid"], sizeof(config.wifi_ssid));
    if (doc["wifi_pass"])   strlcpy(config.wifi_pass, doc["wifi_pass"], sizeof(config.wifi_pass));
    if (doc["server_host"]) strlcpy(config.server_host, doc["server_host"], sizeof(config.server_host));
    if (doc["server_port"]) config.server_port = doc["server_port"];
    if (doc["api_token"])   strlcpy(config.api_token, doc["api_token"], sizeof(config.api_token));
    if (doc["midi_file"])   strlcpy(config.midi_file, doc["midi_file"], sizeof(config.midi_file));
    if (doc["midi_baud"])   config.midi_baud = doc["midi_baud"];
    if (doc.containsKey("port_select")) config.port_select = doc["port_select"];
    if (config.port_select < 0 || config.port_select >= PORT_COUNT) config.port_select = 1;
    if (config.server_port <= 0 || config.server_port > 65535) config.server_port = DEFAULT_SERVER_PORT;

    Serial.println("=== CONFIG ===");
    Serial.printf("  wifi_ssid: %s\n", config.wifi_ssid);
    Serial.printf("  server: %s:%d token:%s\n", config.server_host, config.server_port,
                  strlen(config.api_token) ? "(set)" : "(none)");
    Serial.printf("  midi_file: %s baud:%u port:%d\n", config.midi_file, config.midi_baud, config.port_select);
}

void saveConfig() {
    if (!fs_ok) { Serial.println("FS not ready, config not saved"); return; }
    StaticJsonDocument<1024> doc;
    doc["wifi_ssid"] = config.wifi_ssid;
    doc["wifi_pass"] = config.wifi_pass;
    doc["server_host"] = config.server_host;
    doc["server_port"] = config.server_port;
    doc["api_token"] = config.api_token;
    doc["midi_file"] = config.midi_file;
    doc["midi_baud"] = config.midi_baud;
    doc["port_select"] = config.port_select;
    waitEPDReady();
    File f = LittleFS.open(CONFIG_FILE, FILE_WRITE);
    if (!f) { Serial.println("Failed to save config"); return; }
    serializeJsonPretty(doc, f);
    f.close();
    Serial.println("Config saved");
    logLine("config saved server=%s:%d", config.server_host, config.server_port);
}
