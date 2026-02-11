#include "globals.h"
#include <SD.h>

void drawSettings() {
    Serial.printf("drawSettings() called (cursor=%d, heap=%d)\n", settings_cursor, ESP.getFreeHeap());
    canvas.fillCanvas(0);
    canvas.setTextColor(15);
    canvas.setTextDatum(TL_DATUM);

    canvas.setTextSize(26);
    drawText("=== 設定 ===", 10, 8);

    canvas.setTextSize(22);
    int y = 45;
    int rowH = 50;

    static const char* labels[] = {
        "WiFi SSID", "WiFi Pass", "ICS URL", "ICS User", "ICS Pass",
        "MIDI File", "MIDI URL", "MIDI Baud", "Port", "Alarm Offset",
        "Time Format", "Text Display", "ICS Poll", "Play Duration",
        "Play Repeat", "Notify Topic", "Notify Test", "ICS Update",
        "Sound Test", "Save & Exit"
    };

    static const char* dur_labels[] = {"1曲", "5秒", "10秒", "15秒", "20秒"};
    static const int dur_values[] = {0, 5, 10, 15, 20};
    static const int dur_count = 5;

    int maxVisible = (895 - y) / rowH;

    for (int n = 0; n < maxVisible; n++) {
        int i = settings_cursor + n;
        if (i >= SET_COUNT) break;

        if (n == 0) canvas.fillRect(0, y, 540, rowH, 4);

        canvas.setTextSize(22);
        String val;
        switch (i) {
            case SET_WIFI_SSID: val = config.wifi_ssid; break;
            case SET_WIFI_PASS: val = strlen(config.wifi_pass) > 0 ? "****" : "(empty)"; break;
            case SET_ICS_URL:   val = config.ics_url; break;
            case SET_ICS_USER:  val = strlen(config.ics_user) > 0 ? config.ics_user : "(empty)"; break;
            case SET_ICS_PASS:  val = strlen(config.ics_pass) > 0 ? "****" : "(empty)"; break;
            case SET_MIDI_FILE: val = config.midi_file; break;
            case SET_MIDI_URL:  val = strlen(config.midi_url) > 0 ? config.midi_url : "(empty)"; break;
            case SET_MIDI_BAUD: val = String(config.midi_baud); break;
            case SET_PORT:      val = port_names[config.port_select]; break;
            case SET_ALARM_OFFSET: val = String(config.alarm_offset_default) + "分"; break;
            case SET_TIME_FORMAT: val = config.time_24h ? "24h" : "12h"; break;
            case SET_TEXT_WRAP: val = config.text_wrap ? "折り返し" : "切り詰め"; break;
            case SET_ICS_POLL: val = String(config.ics_poll_min) + "分"; break;
            case SET_PLAY_DURATION: {
                bool found = false;
                for (int d = 0; d < dur_count; d++) {
                    if (dur_values[d] == config.play_duration) { val = dur_labels[d]; found = true; break; }
                }
                if (!found) val = String(config.play_duration) + "秒";
                break;
            }
            case SET_PLAY_REPEAT: val = String(config.play_repeat) + "回"; break;
            case SET_NTFY_TOPIC: val = strlen(config.ntfy_topic) > 0 ? config.ntfy_topic : "(empty)"; break;
            case SET_NTFY_TEST: val = "[実行]"; break;
            case SET_ICS_UPDATE: val = "[実行]"; break;
            case SET_SOUND_TEST: val = "[実行]"; break;
            case SET_SAVE_EXIT:  val = "[実行]"; break;
        }

        drawText(String(labels[i]) + ":", 10, y + 3);
        drawText(val, 10, y + 26);
        y += rowH;
    }

    // ナビゲーションボタン
    int navY = 900;
    int navH = 48;
    canvas.setTextSize(22);

    canvas.drawRect(5, navY, 130, navH, 15);
    canvas.drawString("<<先頭", 25, navY + 12);
    canvas.drawRect(145, navY, 130, navH, 15);
    canvas.drawString("末尾>>", 165, navY + 12);
    canvas.drawRect(285, navY, 130, navH, 15);
    canvas.drawString("戻る", 320, navY + 12);

    canvas.setTextSize(20);
    char footer[32];
    snprintf(footer, sizeof(footer), "[%d/%d]", settings_cursor + 1, SET_COUNT);
    drawText(footer, 440, navY + 14);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void handleSettingsSelect() {
    switch (settings_cursor) {
        case SET_WIFI_SSID:
            keyboard_target = SET_WIFI_SSID;
            keyboard_buffer = config.wifi_ssid;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_WIFI_PASS:
            keyboard_target = SET_WIFI_PASS;
            keyboard_buffer = config.wifi_pass;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_ICS_URL:
            keyboard_target = SET_ICS_URL;
            keyboard_buffer = config.ics_url;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_ICS_USER:
            keyboard_target = SET_ICS_USER;
            keyboard_buffer = config.ics_user;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_ICS_PASS:
            keyboard_target = SET_ICS_PASS;
            keyboard_buffer = config.ics_pass;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_MIDI_FILE:
            scanMidiFiles();
            midi_select_cursor = 0;
            for (int i = 0; i < midi_file_count; i++) {
                if (midi_files[i] == config.midi_file) { midi_select_cursor = i; break; }
            }
            ui_state = UI_MIDI_SELECT; drawMidiSelect(); break;
        case SET_MIDI_URL:
            keyboard_target = SET_MIDI_URL;
            keyboard_buffer = config.midi_url;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_MIDI_BAUD:
            baud_select_cursor = 0;
            for (int i = 0; i < BAUD_OPTION_COUNT; i++) {
                if (baud_options[i] == config.midi_baud) { baud_select_cursor = i; break; }
            }
            ui_state = UI_BAUD_SELECT; drawBaudSelect(); break;
        case SET_PORT:
            port_select_cursor = config.port_select;
            ui_state = UI_PORT_SELECT; drawPortSelect(); break;
        case SET_ALARM_OFFSET:
            config.alarm_offset_default = (config.alarm_offset_default + 5) % 65;
            drawSettings(); break;
        case SET_TIME_FORMAT:
            config.time_24h = !config.time_24h;
            drawSettings(); break;
        case SET_TEXT_WRAP:
            config.text_wrap = !config.text_wrap;
            drawSettings(); break;
        case SET_ICS_POLL: {
            const int poll_opts[] = {5, 10, 15, 30, 60};
            int cur = 0;
            for (int j = 0; j < 5; j++) { if (poll_opts[j] == config.ics_poll_min) { cur = j; break; } }
            cur = (cur + 1) % 5;
            config.ics_poll_min = poll_opts[cur];
            drawSettings(); break;
        }
        case SET_PLAY_DURATION: {
            const int dur_opts[] = {0, 5, 10, 15, 20};
            int cur = 0;
            for (int j = 0; j < 5; j++) { if (dur_opts[j] == config.play_duration) { cur = j; break; } }
            cur = (cur + 1) % 5;
            config.play_duration = dur_opts[cur];
            drawSettings(); break;
        }
        case SET_PLAY_REPEAT:
            config.play_repeat = (config.play_repeat % 5) + 1;
            drawSettings(); break;
        case SET_NTFY_TOPIC:
            keyboard_target = SET_NTFY_TOPIC;
            keyboard_buffer = config.ntfy_topic;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_NTFY_TEST: {
            Serial.println("\n*** NOTIFY TEST ***");
            canvas.fillCanvas(0); canvas.setTextColor(15);
            canvas.setTextDatum(MC_DATUM); canvas.setTextSize(28);
            if (strlen(config.ntfy_topic) == 0) {
                canvas.drawString("通知テスト失敗", 270, 400);
                canvas.setTextSize(22);
                canvas.drawString("Notify Topicが未設定です", 270, 450);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16); delay(2000);
            } else if (WiFi.status() != WL_CONNECTED) {
                canvas.drawString("通知テスト失敗", 270, 400);
                canvas.setTextSize(22);
                canvas.drawString("WiFi未接続です", 270, 450);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16); delay(2000);
            } else {
                canvas.drawString("通知送信中...", 270, 400);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
                sendNtfyNotification("M5Paper Test", "通知テスト - This is a test notification");
                canvas.fillCanvas(0); canvas.setTextColor(15);
                canvas.setTextDatum(MC_DATUM); canvas.setTextSize(28);
                canvas.drawString("通知送信完了", 270, 400);
                canvas.setTextSize(22);
                canvas.drawString(config.ntfy_topic, 270, 450);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16); delay(2000);
            }
            drawSettings(); break;
        }
        case SET_ICS_UPDATE:
            canvas.fillCanvas(0); canvas.setTextColor(15);
            canvas.setTextDatum(MC_DATUM); canvas.setTextSize(28);
            canvas.drawString("ICS取得中...", 270, 280);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            if (WiFi.status() != WL_CONNECTED) connectWiFi();
            fetchAndUpdate();
            drawSettings(); break;
        case SET_SOUND_TEST: {
            Serial.println("\n*** SOUND TEST ***");
            Serial.printf("  MIDI: %s\n", config.midi_file);
            Serial.printf("  Exists: %s\n", SD.exists(config.midi_file) ? "YES" : "NO");
            Serial.printf("  Heap: %d\n", ESP.getFreeHeap());
            if (!SD.exists(config.midi_file)) {
                canvas.fillCanvas(0); canvas.setTextColor(15);
                canvas.setTextDatum(MC_DATUM); canvas.setTextSize(28);
                canvas.drawString("MIDI再生失敗", 270, 400);
                canvas.setTextSize(22);
                canvas.drawString(config.midi_file, 270, 450);
                canvas.drawString("ファイルを確認してください", 270, 490);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16); delay(3000);
                drawSettings(); break;
            }
            int dur = config.play_duration;
            play_duration_ms = dur * 1000;
            int rep = config.play_repeat;
            if (rep < 1) rep = 1;
            play_repeat_remaining = rep;
            play_start_ms = millis();
            playing_event = -1;
            if (startMidiPlayback(config.midi_file)) {
                ui_state = UI_PLAYING;
                canvas.fillCanvas(0); canvas.setTextColor(15);
                canvas.setTextDatum(MC_DATUM);
                canvas.setTextSize(48); canvas.drawString("SOUND TEST", 270, 200);
                canvas.setTextSize(24); canvas.drawString(config.midi_file, 270, 300);
                String info = dur > 0 ? String(dur) + "秒" : "1曲";
                info += " x" + String(rep) + "回";
                canvas.drawString(info, 270, 350);
                canvas.setTextSize(28); canvas.drawString("タップで停止", 270, 450);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            } else {
                canvas.fillCanvas(0); canvas.setTextColor(15);
                canvas.setTextDatum(MC_DATUM); canvas.setTextSize(28);
                canvas.drawString("MIDI再生失敗", 270, 400);
                canvas.setTextSize(22); canvas.drawString(config.midi_file, 270, 450);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16); delay(3000);
                drawSettings();
            }
            break;
        }
        case SET_SAVE_EXIT:
            saveConfig();
            ui_state = UI_LIST;
            scrollToToday(); drawList(); break;
    }
}

void drawMidiSelect() {
    canvas.fillCanvas(0); canvas.setTextColor(15); canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(26);
    drawText("=== MIDIファイル選択 ===", 10, 10);

    canvas.setTextSize(22);
    int y = 55, rowH = 38;
    for (int i = 0; i < midi_file_count && y < 880; i++) {
        if (i == midi_select_cursor) canvas.fillRect(0, y - 2, 540, rowH - 2, 4);
        drawText(midi_files[i], 10, y);
        y += rowH;
    }
    if (midi_file_count == 0) drawText("/midi/ にMIDIファイルなし", 10, 100);

    canvas.setTextSize(20);
    drawText("L:上 R:下 P:選択 タップ:戻る", 10, 920);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawBaudSelect() {
    canvas.fillCanvas(0); canvas.setTextColor(15); canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(26);
    drawText("=== MIDIボーレート選択 ===", 10, 10);

    canvas.setTextSize(30);
    int y = 80, rowH = 55;
    for (int i = 0; i < BAUD_OPTION_COUNT; i++) {
        if (i == baud_select_cursor) canvas.fillRect(0, y - 5, 540, rowH - 5, 4);
        drawText(String(baud_options[i]), 20, y);
        y += rowH;
    }

    canvas.setTextSize(20);
    drawText("L:上 R:下 P:選択", 10, 920);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawPortSelect() {
    canvas.fillCanvas(0); canvas.setTextColor(15); canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(26);
    drawText("=== ポート選択 ===", 10, 10);

    canvas.setTextSize(26);
    int y = 80, rowH = 55;
    for (int i = 0; i < PORT_COUNT; i++) {
        if (i == port_select_cursor) canvas.fillRect(0, y - 5, 540, rowH - 5, 4);
        drawText(port_names[i], 20, y);
        y += rowH;
    }

    canvas.setTextSize(20);
    drawText("L:上 R:下 P:選択", 10, 920);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}
