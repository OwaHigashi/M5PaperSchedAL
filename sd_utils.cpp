#include "globals.h"
#include <SD.h>

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

void reinitSD() {
    Serial.println("SD_REINIT: Attempting SD card reinitialization...");
    SD.end();
    delay(200);

    bool ok = false;
    for (int retry = 0; retry < 3; retry++) {
        if (SD.begin(4)) { ok = true; break; }
        Serial.printf("SD_REINIT: retry %d/3...\n", retry + 1);
        delay(300);
    }

    if (ok) {
        Serial.println("SD_REINIT: Success");
        sd_healthy = true;
    } else {
        Serial.println("SD_REINIT: FAILED - SD card may need physical reset");
        sd_healthy = false;
    }
}

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
