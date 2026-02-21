#include "globals.h"
#include <SD.h>

//==============================================================================
// MIDI コールバック (ファイルスコープ)
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
    Serial.println("MIDI: Stopping all notes...");
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
    Serial.println("MIDI: All notes off + GM Reset sent");
}

//==============================================================================
// MIDIファイルパス取得
//==============================================================================
String getMidiPath(int eventIdx) {
    if (eventIdx < 0 || eventIdx >= event_count) {
        return config.midi_file;
    }

    EventItem& e = events[eventIdx];

    if (e.midi_file[0] == '\0') {
        return config.midi_file;
    }

    if (e.midi_is_url) {
        String localPath;
        if (downloadMidi(e.midi_file, localPath)) {
            return localPath;
        }
        return config.midi_file;
    } else {
        return String("/midi/") + e.midi_file;
    }
}

//==============================================================================
// 再生制御
//==============================================================================
bool startMidiPlayback(const char* filename) {
    waitEPDReady();

    if (!sd_healthy) {
        Serial.println("MIDI: SD not healthy, attempting reinit");
        reinitSD();
        if (!sd_healthy) return false;
    }

    if (!SD.exists(filename)) {
        Serial.printf("MIDI file not found: %s\n", filename);
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
    Serial.printf("stopMidiPlayback called, midi_playing=%d\n", midi_playing);
    if (midi_playing) {
        midi.stop();
        midi.close();
        stopAllNotes();
        midi_playing = false;
        Serial.println("MIDI playback stopped successfully");
    }
}

void finishAlarm() {
    stopMidiPlayback();
    if (playing_event >= 0 && playing_event < event_count) {
        events[playing_event].triggered = true;
    }
    playing_event = -1;
    play_repeat_remaining = 0;
    play_duration_ms = 0;
    ui_state = UI_LIST;
    scrollToToday();
    drawList();

    // アラーム/MIDI待ちで延期されたリブートを実行
    if (reboot_pending) {
        Serial.println("*** REBOOT: deferred reboot after alarm ***");
        Serial.flush();
        delay(100);
        ESP.restart();
    }
}

void updateMidiPlayback() {
    if (!midi_playing) return;

    // 鳴動時間チェック
    if (play_duration_ms > 0 && (millis() - play_start_ms) >= (unsigned long)play_duration_ms) {
        Serial.printf("Play duration reached (%d ms elapsed, limit %d ms)\n",
                      (int)(millis() - play_start_ms), play_duration_ms);
        finishAlarm();
        return;
    }

    if (!midi.update()) {
        play_repeat_remaining--;
        Serial.printf("Play finished, remaining: %d\n", play_repeat_remaining);

        if (play_repeat_remaining > 0) {
            if (play_duration_ms > 0 && (millis() - play_start_ms) >= (unsigned long)play_duration_ms) {
                finishAlarm();
                return;
            }
            stopMidiPlayback();
            String midiPath = getMidiPath(playing_event);
            if (!startMidiPlayback(midiPath.c_str())) {
                finishAlarm();
            }
        } else {
            if (ui_state == UI_PLAYING) {
                finishAlarm();
            }
        }
    }
}
