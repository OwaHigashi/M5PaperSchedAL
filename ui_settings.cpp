#include "globals.h"
#include "ui_colors.h"

void drawSettings(bool fast) {
    Serial.printf("drawSettings() called (cursor=%d, heap=%d)\n", settings_cursor, ESP.getFreeHeap());
    canvas.fillCanvas(COL_SETTINGS_BG);
    canvas.setTextColor(COL_SETTINGS_TEXT);
    canvas.setTextDatum(TL_DATUM);

    canvas.setTextSize(30);
    drawTextBold("=== 設定 ===", 10, 6, 1);

    canvas.setTextSize(26);
    int y = 46;
    int rowH = 60;

    static const char* labels[] = {
        "ホストと再同期", "Server Host", "Server Port", "WiFi SSID", "WiFi Pass",
        "MIDI File (ローカル既定)", "MIDI Baud", "Port",
        "Sound Test", "スクリーンショット送信", "Save & Exit"
    };

    int maxVisible = (890 - y) / rowH;

    for (int n = 0; n < maxVisible; n++) {
        int i = settings_cursor + n;
        if (i >= SET_COUNT) break;

        if (n == 0) canvas.fillRect(0, y, 540, rowH, COL_SETTINGS_CURSOR);

        canvas.setTextSize(26);
        String val;
        switch (i) {
            case SET_SYNC_NOW: {
                char b[64];
                snprintf(b, sizeof(b), "[実行] host:%s rev:%ld/%ld ev:%d", host_online ? "up" : "DOWN", local_rev, host_rev, event_count);
                val = b; break;
            }
            case SET_SERVER_HOST: val = config.server_host; break;
            case SET_SERVER_PORT: val = String(config.server_port); break;
            case SET_WIFI_SSID: val = config.wifi_ssid; break;
            case SET_WIFI_PASS: val = strlen(config.wifi_pass) > 0 ? "****" : "(empty)"; break;
            case SET_MIDI_FILE: val = config.midi_file; break;
            case SET_MIDI_BAUD: val = String(config.midi_baud); break;
            case SET_PORT:      val = port_names[config.port_select]; break;
            case SET_SOUND_TEST: val = "[実行]"; break;
            case SET_SCREENSHOT: val = "[実行]"; break;
            case SET_SAVE_EXIT:  val = "[実行]"; break;
        }

        String label = String(labels[i]) + ":";
        drawTextBold(label, 10, y + 2, 1);
        drawTextBold(val, 10, y + 30, 1);
        y += rowH;
    }

    // ナビゲーションボタン
    int navY = 900;
    int navH = 50;
    canvas.setTextSize(26);

    canvas.drawRect(5, navY, 130, navH, COL_SETTINGS_BTN);
    drawTextBold("<<先頭", 20, navY + 10, 1);
    canvas.drawRect(145, navY, 130, navH, COL_SETTINGS_BTN);
    drawTextBold("末尾>>", 160, navY + 10, 1);
    canvas.drawRect(285, navY, 130, navH, COL_SETTINGS_BTN);
    drawTextBold("戻る", 318, navY + 10, 1);

    canvas.setTextSize(24);
    char footer[32];
    snprintf(footer, sizeof(footer), "[%d/%d]", settings_cursor + 1, SET_COUNT);
    drawTextBold(footer, 430, navY + 12, 1);

    unsigned long t0 = millis();
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    Serial.printf("[SETTINGS] pushCanvas took %lu ms\n", millis() - t0);
}

void handleSettingsSelect() {
    switch (settings_cursor) {
        case SET_SYNC_NOW:
            canvas.fillCanvas(COL_SETTINGS_BG); canvas.setTextColor(COL_SETTINGS_TEXT);
            canvas.setTextDatum(MC_DATUM); canvas.setTextSize(28);
            drawTextBold("ホストと同期中...", 270, 280, 1);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            if (WiFi.status() != WL_CONNECTED) connectWiFi();
            sendHeartbeat(true);
            fetchAndUpdate();
            ui_state = UI_LIST;
            scrollToToday(); drawList(); break;
        case SET_SERVER_HOST:
            keyboard_target = SET_SERVER_HOST;
            keyboard_buffer = config.server_host;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_SERVER_PORT:
            keyboard_target = SET_SERVER_PORT;
            keyboard_buffer = String(config.server_port);
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_WIFI_SSID:
            keyboard_target = SET_WIFI_SSID;
            keyboard_buffer = config.wifi_ssid;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_WIFI_PASS:
            keyboard_target = SET_WIFI_PASS;
            keyboard_buffer = config.wifi_pass;
            ui_state = UI_KEYBOARD; drawKeyboard(); break;
        case SET_MIDI_FILE:
            scanMidiFiles();
            midi_select_cursor = 0;
            for (int i = 0; i < midi_file_count; i++) {
                if (midi_files[i] == config.midi_file) { midi_select_cursor = i; break; }
            }
            ui_state = UI_MIDI_SELECT; drawMidiSelect(); break;
        case SET_MIDI_BAUD:
            baud_select_cursor = 0;
            for (int i = 0; i < BAUD_OPTION_COUNT; i++) {
                if (baud_options[i] == config.midi_baud) { baud_select_cursor = i; break; }
            }
            ui_state = UI_BAUD_SELECT; drawBaudSelect(); break;
        case SET_PORT:
            port_select_cursor = config.port_select;
            ui_state = UI_PORT_SELECT; drawPortSelect(); break;
        case SET_SCREENSHOT:
            drawSettings();
            saveScreenshot();
            break;
        case SET_SOUND_TEST: {
            Serial.printf("\n*** SOUND TEST *** %s\n", config.midi_file);
            const char* name = strrchr(config.midi_file, '/');
            name = name ? name + 1 : config.midi_file;
            if (!startCommandPlay(name, false, config.play_duration, config.play_repeat, "", "")) {
                canvas.fillCanvas(COL_SETTINGS_BG); canvas.setTextColor(COL_SETTINGS_TEXT);
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
    canvas.fillCanvas(COL_SETTINGS_BG); canvas.setTextColor(COL_SETTINGS_TEXT); canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(30);
    drawTextBold("=== MIDIファイル選択 ===", 10, 8, 1);

    canvas.setTextSize(26);
    int y = 50, rowH = 44;
    for (int i = 0; i < midi_file_count && y < 880; i++) {
        if (i == midi_select_cursor) canvas.fillRect(0, y - 2, 540, rowH - 2, COL_SETTINGS_CURSOR);
        drawTextBold(midi_files[i], 10, y, 1);
        y += rowH;
    }
    if (midi_file_count == 0) drawTextBold("/midi/ にMIDIファイルなし", 10, 100, 1);

    canvas.setTextSize(26);
    drawTextBold("L:上 R:下 P:選択 タップ:戻る", 10, 920, 1);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawBaudSelect() {
    canvas.fillCanvas(COL_SETTINGS_BG); canvas.setTextColor(COL_SETTINGS_TEXT); canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(30);
    drawTextBold("=== MIDIボーレート選択 ===", 10, 8, 1);

    canvas.setTextSize(30);
    int y = 80, rowH = 55;
    for (int i = 0; i < BAUD_OPTION_COUNT; i++) {
        if (i == baud_select_cursor) canvas.fillRect(0, y - 5, 540, rowH - 5, COL_SETTINGS_CURSOR);
        drawTextBold(String(baud_options[i]), 20, y, 1);
        y += rowH;
    }

    canvas.setTextSize(26);
    drawTextBold("L:上 R:下 P:選択", 10, 920, 1);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawPortSelect() {
    canvas.fillCanvas(COL_SETTINGS_BG); canvas.setTextColor(COL_SETTINGS_TEXT); canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(30);
    drawTextBold("=== ポート選択 ===", 10, 8, 1);

    canvas.setTextSize(28);
    int y = 80, rowH = 55;
    for (int i = 0; i < PORT_COUNT; i++) {
        if (i == port_select_cursor) canvas.fillRect(0, y - 5, 540, rowH - 5, COL_SETTINGS_CURSOR);
        drawTextBold(port_names[i], 20, y, 1);
        y += rowH;
    }

    canvas.setTextSize(26);
    drawTextBold("L:上 R:下 P:選択", 10, 920, 1);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}
