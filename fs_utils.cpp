#include "globals.h"

//==============================================================================
// EPD描画完了待ち
//   pushCanvas()は非同期で戻ることがある。IT8951が無応答だとCheckAFSR()が
//   ブロックするため Task WDT で回復する (setup参照)。
//==============================================================================
void waitEPDReady() {
    M5.EPD.CheckAFSR();
}

//==============================================================================
// LittleFS (内蔵フラッシュ) 初期化。SDカードは使用しない。
//   data/ 以下を `pio run -t uploadfs` で書き込む: /config.json /fonts/ipaexg.ttf /midi/*.mid
//==============================================================================
bool initFS() {
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed - formatting");
        if (!LittleFS.begin(true)) {
            Serial.println("LittleFS format failed");
            fs_ok = false;
            return false;
        }
    }
    fs_ok = true;
    if (!LittleFS.exists(MIDI_DL_DIR)) LittleFS.mkdir(MIDI_DL_DIR);
    Serial.printf("LittleFS: %u / %u bytes used\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    return true;
}

size_t fsUsed()  { return fs_ok ? LittleFS.usedBytes() : 0; }
size_t fsTotal() { return fs_ok ? LittleFS.totalBytes() : 0; }

//==============================================================================
// MIDIファイルスキャン (/midi と /midi-dl)
//==============================================================================
static void scanDir(const char* dirPath) {
    File dir = LittleFS.open(dirPath);
    if (!dir || !dir.isDirectory()) return;
    while (midi_file_count < 32) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String name = entry.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!entry.isDirectory() &&
            (name.endsWith(".mid") || name.endsWith(".MID") ||
             name.endsWith(".midi") || name.endsWith(".MIDI"))) {
            midi_files[midi_file_count++] = String(dirPath) + "/" + name;
        }
        entry.close();
    }
    dir.close();
}

void scanMidiFiles() {
    midi_file_count = 0;
    if (!fs_ok) return;
    scanDir(MIDI_DIR);
    scanDir(MIDI_DL_DIR);
    Serial.printf("Found %d MIDI files\n", midi_file_count);
}
