/*******************************************************************************
 * M5Paper GPIO Switch Test
 * 
 * M5Paperの側面スイッチ(L/R/P)のGPIOピンを特定するためのテストスケッチ
 * シリアルモニタで押されたボタンを確認できます
 ******************************************************************************/

#include <M5EPD.h>

// テスト対象のGPIOピン（M5Paper v1.1の一般的な値）
const int test_pins[] = {36, 37, 38, 39, 2, 13, 15, 32, 33, 34, 35};
const int pin_count = sizeof(test_pins) / sizeof(test_pins[0]);

bool pin_states[20];

void setup() {
    Serial.begin(115200);
    M5.begin();
    
    Serial.println("\n=== M5Paper GPIO Switch Test ===");
    Serial.println("Press L, R, or P switch to see which GPIO changes");
    Serial.println("");
    
    // 全ピンをINPUT_PULLUPに設定
    for (int i = 0; i < pin_count; i++) {
        pinMode(test_pins[i], INPUT_PULLUP);
        pin_states[i] = digitalRead(test_pins[i]);
    }
    
    Serial.println("Monitoring GPIOs: ");
    for (int i = 0; i < pin_count; i++) {
        Serial.printf("GPIO%d ", test_pins[i]);
    }
    Serial.println("\n");
}

void loop() {
    for (int i = 0; i < pin_count; i++) {
        bool current = digitalRead(test_pins[i]);
        if (current != pin_states[i]) {
            Serial.printf("GPIO%d: %s\n", 
                         test_pins[i], 
                         current ? "HIGH (released)" : "LOW (pressed)");
            pin_states[i] = current;
        }
    }
    delay(10);
}
