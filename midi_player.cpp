#include "globals.h"

//==============================================================================
// MIDI コールバック
//==============================================================================
static void midiSendCallback(uint8_t* data, uint16_t len) {
    if (len > 0) Serial2.write(data, len);
}

static void sysexCallback(uint8_t* data, uint32_t len) {
    if (len > 0) Serial2.write(data, len);
}

static void sendCC(uint8_t ch, uint8_t cc, uint8_t val) {
    uint8_t msg[3] = {(uint8_t)(0xB0 | (ch & 0x0F)), cc, val};
    Serial2.write(msg, 3);
}

//==============================================================================
// 全チャンネル停止 + GM Reset
//==============================================================================
void stopAllNotes() {
    for (int ch = 0; ch < 16; ch++) {
        sendCC(ch, 120, 0);  // All Sound Off
        sendCC(ch, 123, 0);  // All Notes Off
    }
    Serial2.flush();
    delay(50);
    uint8_t gmReset[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
    Serial2.write(gmReset, sizeof(gmReset));
    Serial2.flush();
    delay(100);
}

//==============================================================================
// MIDIファイルパス解決
//   midi_is_url: ホストから /api/v1/midi/<name> で取得して /midi-dl にキャッシュ
//   それ以外   : /midi/<name> (LittleFS)
//   見つからなければホスト既定 (midi_default) → 端末既定 (config.midi_file)
//==============================================================================
static String resolveMidi(const char* name, bool isUrl) {
    if (name && name[0]) {
        if (isUrl) {
            String localPath;
            if (downloadMidi(name, localPath)) return localPath;
        } else {
            String p = String(MIDI_DIR) + "/" + name;
            if (LittleFS.exists(p.c_str())) return p;
            String q = String(MIDI_DL_DIR) + "/" + name;
            if (LittleFS.exists(q.c_str())) return q;
        }
        Serial.printf("MIDI '%s' not available, falling back\n", name);
    }
    // ホストの既定MIDI (ホストから取得できる)
    if (config.midi_default[0]) {
        String p = String(MIDI_DIR) + "/" + config.midi_default;
        if (LittleFS.exists(p.c_str())) return p;
        String localPath;
        if (downloadMidi(config.midi_default, localPath)) return localPath;
    }
    return config.midi_file;
}

String getMidiPath(int eventIdx) {
    if (play_file_override[0]) return play_file_override;
    if (eventIdx < 0 || eventIdx >= event_count) return config.midi_file;
    EventItem& e = events[eventIdx];
    return resolveMidi(e.midi_file, e.midi_is_url);
}

//==============================================================================
// 再生制御
//==============================================================================
bool startMidiPlayback(const char* filename) {
    if (!fs_ok || !LittleFS.exists(filename)) {
        Serial.printf("MIDI file not found: %s\n", filename);
        logLine("midi missing %s", filename);
        return false;
    }
    if (!midi.load(filename)) {
        Serial.println("MIDI load failed");
        return false;
    }
    midi.setMidiCallback(midiSendCallback);
    midi.setSysExCallback(sysexCallback);
    midi.play();
    midi_playing = true;
    Serial.printf("MIDI playback started: %s\n", filename);
    return true;
}

void stopMidiPlayback() {
    if (midi_playing) {
        midi.stop();
        midi.close();
        stopAllNotes();
        midi_playing = false;
    }
}

// ホストコマンド / サウンドテストからの再生
bool startCommandPlay(const char* midiName, bool isUrl, int durationSec, int repeat,
                      const char* alarm_id, const char* event_id) {
    String path = resolveMidi(midiName, isUrl);
    strlcpy(play_file_override, path.c_str(), sizeof(play_file_override));
    int dur = durationSec >= 0 ? durationSec : config.play_duration;
    int rep = repeat >= 0 ? repeat : config.play_repeat;
    if (rep < 1) rep = 1;
    play_duration_ms = dur * 1000;
    play_repeat_remaining = rep;
    play_start_ms = millis();
    playing_event = findEventById(event_id);
    playing_alarm_idx = -1;
    strlcpy(playing_alarm_id, alarm_id ? alarm_id : "", sizeof(playing_alarm_id));
    if (playing_event >= 0 && playing_alarm_id[0]) {
        // ホストから明示発火: 該当スロットを特定 (二重鳴動防止のため triggered 確認)
        for (int k = 0; k < events[playing_event].alarm_count; k++) {
            char aid[40]; alarmIdOf(events[playing_event], k, aid, sizeof(aid));
            if (strcmp(aid, playing_alarm_id) == 0) {
                if (events[playing_event].triggered[k]) { play_file_override[0] = '\0'; return true; }  // 既に鳴動済
                playing_alarm_idx = k; break;
            }
        }
    }
    if (!startMidiPlayback(path.c_str())) { play_file_override[0] = '\0'; return false; }
    ui_state = UI_PLAYING;
    if (playing_event >= 0) drawPlaying(playing_event);
    else drawPlayingGeneric(playing_alarm_id[0] ? "ALARM!" : "SOUND TEST", path.c_str());
    logLine("play cmd %s dur=%d rep=%d", path.c_str(), dur, rep);
    return true;
}

void finishAlarm() {
    stopMidiPlayback();
    if (playing_event >= 0 && playing_event < event_count) {
        if (playing_alarm_idx >= 0 && playing_alarm_idx < events[playing_event].alarm_count) {
            events[playing_event].triggered[playing_alarm_idx] = true;
        }
    }
    char aid[40];
    strlcpy(aid, playing_alarm_id, sizeof(aid));
    playing_event = -1;
    playing_alarm_idx = -1;
    playing_alarm_id[0] = '\0';
    play_file_override[0] = '\0';
    play_repeat_remaining = 0;
    play_duration_ms = 0;
    ui_state = UI_LIST;
    scrollToToday();
    drawList();
    if (aid[0]) ackAlarm(aid, "done");     // ホストが triggered を確定 (未達なら保持して再送)
    uiEventPush("alarm", 0, 0, aid[0] ? aid : "test-end");

    if (reboot_pending) {
        Serial.println("*** REBOOT: deferred reboot after alarm ***");
        safeReboot();
    }
}

void updateMidiPlayback() {
    if (!midi_playing) return;

    if (play_duration_ms > 0 && (millis() - play_start_ms) >= (unsigned long)play_duration_ms) {
        finishAlarm();
        return;
    }
    if (!midi.update()) {
        play_repeat_remaining--;
        if (play_repeat_remaining > 0) {
            if (play_duration_ms > 0 && (millis() - play_start_ms) >= (unsigned long)play_duration_ms) {
                finishAlarm();
                return;
            }
            stopMidiPlayback();
            String midiPath = getMidiPath(playing_event);
            if (!startMidiPlayback(midiPath.c_str())) finishAlarm();
        } else {
            finishAlarm();
        }
    }
}
