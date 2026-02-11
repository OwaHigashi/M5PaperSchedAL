#include "globals.h"
#include <SD.h>

#define SD_CS_PIN 4

//==============================================================================
// EPD描画完了待ち
// pushCanvas()はEPDの更新を非同期で開始して戻ることがある。
// EPDとSDは同じSPIバスを共有しているため、EPDがバスを使用中に
// SD操作を行うと衝突してSDがロックする。
// この関数を全てのSD操作の前に呼ぶことで衝突を防ぐ。
//==============================================================================
void waitEPDReady() {
    M5.EPD.CheckAFSR();  // IT8951 コントローラの準備完了を待つ
}

//==============================================================================
// SD初期化（リトライ付き）
//==============================================================================
bool initSD() {
    waitEPDReady();

    for (int retry = 0; retry < 5; retry++) {
        if (SD.begin(SD_CS_PIN)) {
            if (retry > 0) Serial.printf("SD_INIT: OK on attempt %d\n", retry + 1);
            else Serial.println("SD_INIT: OK");
            sd_healthy = true;
            return true;
        }
        Serial.printf("SD_INIT: attempt %d/5 failed\n", retry + 1);
        SD.end();
        delay(300 + retry * 200);
    }

    Serial.println("SD_INIT: All attempts failed - need SD card removal");
    sd_healthy = false;
    return false;
}

//==============================================================================
// SDカード健全性チェック
//==============================================================================
bool checkSDHealth() {
    waitEPDReady();

    File f = SD.open(CONFIG_FILE, FILE_READ);
    if (!f) {
        Serial.println("SD_CHECK: Cannot open config file - SD may be unhealthy");
        return false;
    }
    int b = f.read();
    f.close();
    if (b < 0) {
        Serial.println("SD_CHECK: Cannot read from config file");
        return false;
    }
    return true;
}

//==============================================================================
// SD再初期化
//==============================================================================
void reinitSD() {
    waitEPDReady();

    Serial.println("SD_REINIT: Attempting SD card reinitialization...");
    SD.end();
    delay(500);

    for (int retry = 0; retry < 3; retry++) {
        if (SD.begin(SD_CS_PIN)) {
            Serial.println("SD_REINIT: Success");
            sd_healthy = true;
            return;
        }
        Serial.printf("SD_REINIT: retry %d/3 failed\n", retry + 1);
        SD.end();
        delay(500);
    }

    Serial.println("SD_REINIT: FAILED - SD card needs physical removal");
    sd_healthy = false;
}

//==============================================================================
// MIDIファイルスキャン
//==============================================================================
void scanMidiFiles() {
    waitEPDReady();

    midi_file_count = 0;
    File dir = SD.open(MIDI_DIR);
    if (!dir) {
        Serial.println("MIDI dir not found");
        return;
    }

    while (midi_file_count < 32) {
        File entry = dir.openNextFile();
        if (!entry) break;

        String name = entry.name();
        if (!entry.isDirectory() &&
            (name.endsWith(".mid") || name.endsWith(".MID") ||
             name.endsWith(".midi") || name.endsWith(".MIDI"))) {
            midi_files[midi_file_count] = String(MIDI_DIR) + "/" + name;
            midi_file_count++;
        }
        entry.close();
    }
    dir.close();
    Serial.printf("Found %d MIDI files\n", midi_file_count);
}
