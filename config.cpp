#include "globals.h"
#include <SD.h>
#include <ArduinoJson.h>

void loadConfig() {
    waitEPDReady();
    // デフォルト値（初回起動用）
    strcpy(config.wifi_ssid, "your_wifi_ssid");
    strcpy(config.wifi_pass, "your_wifi_password");
    strcpy(config.ics_url, "https://example.com/calendar.ics");
    strcpy(config.ics_user, "");
    strcpy(config.ics_pass, "");
    strcpy(config.midi_file, "/midi/alarm.mid");
    strcpy(config.midi_url, "");
    strcpy(config.ntfy_topic, "");
    config.midi_baud = DEFAULT_MIDI_BAUD;
    config.alarm_offset_default = DEFAULT_ALARM_OFFSET;
    config.port_select = 1;    // デフォルトPort B
    config.time_24h = true;
    config.text_wrap = false;
    config.ics_poll_min = 30;
    config.play_duration = 0;  // 0=1曲
    config.play_repeat = 1;
    config.max_events = 99;
    config.max_desc_bytes = 500;
    config.min_free_heap = 40;

    if (!SD.exists(CONFIG_FILE)) {
        Serial.println("Config not found, using defaults");
        return;
    }

    File f = SD.open(CONFIG_FILE, FILE_READ);
    if (!f) return;

    StaticJsonDocument<1280> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.println("JSON parse error");
        return;
    }

    if (doc["wifi_ssid"]) strlcpy(config.wifi_ssid, doc["wifi_ssid"], sizeof(config.wifi_ssid));
    if (doc["wifi_pass"]) strlcpy(config.wifi_pass, doc["wifi_pass"], sizeof(config.wifi_pass));
    if (doc["ics_url"])   strlcpy(config.ics_url, doc["ics_url"], sizeof(config.ics_url));
    if (doc["ics_user"])  strlcpy(config.ics_user, doc["ics_user"], sizeof(config.ics_user));
    if (doc["ics_pass"])  strlcpy(config.ics_pass, doc["ics_pass"], sizeof(config.ics_pass));
    if (doc["midi_file"]) strlcpy(config.midi_file, doc["midi_file"], sizeof(config.midi_file));
    if (doc["midi_url"])  strlcpy(config.midi_url, doc["midi_url"], sizeof(config.midi_url));
    if (doc["ntfy_topic"]) strlcpy(config.ntfy_topic, doc["ntfy_topic"], sizeof(config.ntfy_topic));
    if (doc["midi_baud"]) config.midi_baud = doc["midi_baud"];
    if (doc["alarm_offset"]) config.alarm_offset_default = doc["alarm_offset"];
    if (doc["port_select"]) config.port_select = doc["port_select"];
    if (doc.containsKey("time_24h")) config.time_24h = doc["time_24h"];
    if (doc.containsKey("text_wrap")) config.text_wrap = doc["text_wrap"];
    if (doc["ics_poll_min"]) config.ics_poll_min = doc["ics_poll_min"];

    if (config.ics_poll_min < 5) {
        Serial.printf("Config: ics_poll_min=%d is too small, setting to 5\n", config.ics_poll_min);
        config.ics_poll_min = 5;
    }
    if (doc.containsKey("play_duration")) config.play_duration = doc["play_duration"];
    if (doc["play_repeat"]) config.play_repeat = doc["play_repeat"];
    if (doc["max_events"]) config.max_events = doc["max_events"];
    if (doc["max_desc_bytes"]) config.max_desc_bytes = doc["max_desc_bytes"];
    if (doc["min_free_heap"]) config.min_free_heap = doc["min_free_heap"];

    if (config.max_events < 10) config.max_events = 10;
    if (config.max_events > MAX_EVENTS - 1) config.max_events = MAX_EVENTS - 1;
    if (config.max_desc_bytes < 100) config.max_desc_bytes = 100;
    if (config.min_free_heap < 20) config.min_free_heap = 20;

    Serial.println("Config loaded");
}

void saveConfig() {
    waitEPDReady();
    StaticJsonDocument<1280> doc;
    doc["wifi_ssid"] = config.wifi_ssid;
    doc["wifi_pass"] = config.wifi_pass;
    doc["ics_url"] = config.ics_url;
    doc["ics_user"] = config.ics_user;
    doc["ics_pass"] = config.ics_pass;
    doc["midi_file"] = config.midi_file;
    doc["midi_url"] = config.midi_url;
    doc["ntfy_topic"] = config.ntfy_topic;
    doc["midi_baud"] = config.midi_baud;
    doc["alarm_offset"] = config.alarm_offset_default;
    doc["port_select"] = config.port_select;
    doc["time_24h"] = config.time_24h;
    doc["text_wrap"] = config.text_wrap;
    doc["ics_poll_min"] = config.ics_poll_min;
    doc["play_duration"] = config.play_duration;
    doc["play_repeat"] = config.play_repeat;
    doc["max_events"] = config.max_events;
    doc["max_desc_bytes"] = config.max_desc_bytes;
    doc["min_free_heap"] = config.min_free_heap;

    File f = SD.open(CONFIG_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("Failed to save config");
        return;
    }
    serializeJson(doc, f);
    f.flush();
    f.close();
    Serial.println("Config saved");
}
