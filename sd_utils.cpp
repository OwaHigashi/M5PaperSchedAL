#include "globals.h"
#include <SD.h>
#include <SPI.h>

#define SD_CS_PIN  4
#define SD_SCK_PIN 14   // M5Paper HSPI SCK

//==============================================================================
// SDカード GPIOビットバングリセット
//
// M5PaperはEPDとSDが同じSPIバスを共有しており、SPIライブラリの
// beginTransaction/endTransactionを直接触るとピン設定が壊れることがある。
// そのため、GPIO直接制御（ビットバング）でSDカードのリセットを行う。
//
// SD仕様: CS=HIGHの状態で74クロック以上送信 → CS=LOWでCMD0送信
// → SDカードがSPIモードのIDLE状態に戻る
//==============================================================================

// GPIO SCKピンを手動トグルしてクロック1個を生成
static void bitBangClock() {
    digitalWrite(SD_SCK_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(SD_SCK_PIN, LOW);
    delayMicroseconds(5);
}

// GPIO MOSI(12)で1バイト送信（MSBファースト）
static void bitBangSendByte(uint8_t data) {
    const int MOSI_PIN = 12;  // M5Paper HSPI MOSI
    for (int bit = 7; bit >= 0; bit--) {
        digitalWrite(MOSI_PIN, (data >> bit) & 1);
        bitBangClock();
    }
}

static void resetSDBus() {
    Serial.println("SD_BUS: GPIO bit-bang reset...");

    // SDライブラリを解放
    SD.end();
    delay(100);

    // GPIOを出力モードに設定
    pinMode(SD_CS_PIN, OUTPUT);
    pinMode(SD_SCK_PIN, OUTPUT);
    pinMode(12, OUTPUT);  // MOSI
    digitalWrite(SD_CS_PIN, HIGH);
    digitalWrite(SD_SCK_PIN, LOW);
    digitalWrite(12, HIGH);  // MOSI=HIGHがアイドル
    delay(10);

    // Step 1: CS=HIGHでダミークロック160個（パワーアップシーケンス）
    for (int i = 0; i < 160; i++) {
        bitBangClock();
    }

    // Step 2: CS=LOWにしてCMD0送信
    digitalWrite(SD_CS_PIN, LOW);
    delayMicroseconds(10);

    bitBangSendByte(0x40 | 0);   // CMD0
    bitBangSendByte(0x00);        // arg[31:24]
    bitBangSendByte(0x00);        // arg[23:16]
    bitBangSendByte(0x00);        // arg[15:8]
    bitBangSendByte(0x00);        // arg[7:0]
    bitBangSendByte(0x95);        // CRC7 + stop bit

    // R1応答読み取り（MISO=GPIO13）
    pinMode(13, INPUT);
    uint8_t response = 0xFF;
    for (int i = 0; i < 16; i++) {
        uint8_t r = 0;
        for (int bit = 7; bit >= 0; bit--) {
            digitalWrite(SD_SCK_PIN, HIGH);
            delayMicroseconds(5);
            r |= (digitalRead(13) << bit);
            digitalWrite(SD_SCK_PIN, LOW);
            delayMicroseconds(5);
        }
        if (r != 0xFF) { response = r; break; }
    }

    Serial.printf("SD_BUS: CMD0 response = 0x%02X", response);
    if (response == 0x01) Serial.println(" (IDLE - OK)");
    else if (response == 0xFF) Serial.println(" (no response)");
    else Serial.printf(" (0x%02X)\n", response);

    // CS=HIGH に戻して追加クロック
    digitalWrite(SD_CS_PIN, HIGH);
    for (int i = 0; i < 16; i++) bitBangClock();

    // ★ GPIOモードを入力に戻す（SD.begin()がSPIとして再設定できるように）
    pinMode(SD_SCK_PIN, INPUT);
    pinMode(12, INPUT);
    pinMode(13, INPUT);
    delay(100);

    Serial.println("SD_BUS: Reset complete");
}

//==============================================================================
// SD初期化（GPIOリセット付き）
//==============================================================================
bool initSD() {
    // まず通常のSD.begin()を試す
    if (SD.begin(SD_CS_PIN)) {
        Serial.println("SD_INIT: OK (first attempt)");
        sd_healthy = true;
        return true;
    }

    // 失敗 → GPIOリセット + リトライ
    Serial.println("SD_INIT: First attempt failed, trying GPIO reset...");

    for (int retry = 0; retry < 5; retry++) {
        resetSDBus();
        if (SD.begin(SD_CS_PIN)) {
            Serial.printf("SD_INIT: Success on retry %d\n", retry + 1);
            sd_healthy = true;
            return true;
        }
        Serial.printf("SD_INIT: retry %d/5 failed\n", retry + 1);
    }

    Serial.println("SD_INIT: All retries failed - need physical card removal");
    sd_healthy = false;
    return false;
}

//==============================================================================
// SDカード健全性チェック
//==============================================================================
bool checkSDHealth() {
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
// SD再初期化（GPIOリセット付き）
//==============================================================================
void reinitSD() {
    Serial.println("SD_REINIT: Attempting SD card reinitialization...");

    for (int retry = 0; retry < 3; retry++) {
        resetSDBus();
        if (SD.begin(SD_CS_PIN)) {
            Serial.println("SD_REINIT: Success");
            sd_healthy = true;
            return;
        }
        Serial.printf("SD_REINIT: retry %d/3 failed\n", retry + 1);
    }

    Serial.println("SD_REINIT: FAILED - SD card needs physical removal");
    sd_healthy = false;
}

//==============================================================================
// MIDIファイルスキャン
//==============================================================================
void scanMidiFiles() {
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
