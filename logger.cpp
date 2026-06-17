#include "globals.h"
#include <SD.h>
#include <stdarg.h>

//==============================================================================
// 縮約診断ログ
//   - 常に Serial へ "LOG <時刻> ..." の1行で出力（既存の冗長ログとは別系統で短い）
//   - config.sd_log_enabled が true のときのみ SD /log/sched.log にも追記
//   - サイズが LOG_MAX_BYTES を超えたら sched.1.log へローテーション（2世代）
//   SD/EPDはSPI共有のため、SD書き込み前に waitEPDReady() で描画完了を待つ。
//==============================================================================

#define LOG_DIR        "/log"
#define LOG_PATH       "/log/sched.log"
#define LOG_PATH_OLD   "/log/sched.1.log"
#define LOG_MAX_BYTES  (2 * 1024 * 1024)   // 2MB毎にローテーション（縮約ログで約10週間/世代）

static void rotateIfNeeded() {
    File f = SD.open(LOG_PATH, FILE_READ);
    if (!f) return;
    size_t sz = f.size();
    f.close();
    if (sz < LOG_MAX_BYTES) return;
    SD.remove(LOG_PATH_OLD);
    SD.rename(LOG_PATH, LOG_PATH_OLD);
}

void logLine(const char* fmt, ...) {
    char buf[256];

    // 先頭に時刻（NTP未同期時は millis ベース）
    int n = 0;
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        n = snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d:%02d ",
                     tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        n = snprintf(buf, sizeof(buf), "+%lus ", millis() / 1000);
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    // Serial へは常に出す
    Serial.print("LOG ");
    Serial.println(buf);

    // SD へはオプション
    if (!config.sd_log_enabled || !sd_healthy) return;

    waitEPDReady();
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
    rotateIfNeeded();
    File f = SD.open(LOG_PATH, FILE_APPEND);
    if (f) {
        f.println(buf);
        f.close();
    }
}

//==============================================================================
// USB(シリアル)経由のログ取得/削除コマンド
//   ホスト側はCOM14へ改行終端のコマンドを送る。応答は LOGDATA ヘッダ + 生バイト + LOGEND で
//   フレーミングされるので、他のデバッグ出力と混ざっても安全に切り出せる。
//
//   LOGSIZE                 → "LOGSIZE sched=<bytes> old=<bytes>"
//   LOGREAD <offset> <len>  → /log/sched.log を offset から len バイト読む（len上限8192）
//   LOGREADOLD <off> <len>  → ローテーション済み /log/sched.1.log を読む
//   LOGCLEAR                → 両ファイル削除
//   LOGHELP                 → コマンド一覧
//
//   応答(LOGREAD系):
//     LOGDATA off=<o> len=<actual> size=<total> eof=<0|1>
//     <actual バイトの生データ>
//     LOGEND
//   ホストは header の actual バイトをバイナリ安全に読み取り→LOGEND まで読み飛ばす。
//   eof=1 まで offset を進めればチャンク取得で全体を再構成できる。
//==============================================================================

#define LOG_READ_MAX  8192

static void cmdReadChunk(const char* path, long offset, long len) {
    if (len <= 0 || len > LOG_READ_MAX) len = LOG_READ_MAX;
    waitEPDReady();
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("LOGDATA off=%ld len=0 size=0 eof=1 err=noopen path=%s\n", offset, path);
        Serial.println("LOGEND");
        return;
    }
    long size = (long)f.size();
    if (offset < 0) offset = 0;
    if (offset > size) offset = size;
    long actual = size - offset;
    if (actual > len) actual = len;
    int eof = (offset + actual >= size) ? 1 : 0;
    f.seek(offset);
    Serial.printf("LOGDATA off=%ld len=%ld size=%ld eof=%d\n", offset, actual, size, eof);
    uint8_t buf[512];
    long toread = actual;
    while (toread > 0) {
        int chunk = (toread > (long)sizeof(buf)) ? (int)sizeof(buf) : (int)toread;
        int n = f.read(buf, chunk);
        if (n <= 0) break;
        Serial.write(buf, n);
        toread -= n;
    }
    f.close();
    Serial.println();          // データとLOGENDの区切り（actualバイトは上で確定済み）
    Serial.println("LOGEND");
}

static void handleLogCommand(const char* line) {
    long a = 0, b = 0;
    if (strncmp(line, "LOGSIZE", 7) == 0) {
        long s1 = 0, s2 = 0;
        File f = SD.open(LOG_PATH, FILE_READ);    if (f) { s1 = (long)f.size(); f.close(); }
        File g = SD.open(LOG_PATH_OLD, FILE_READ); if (g) { s2 = (long)g.size(); g.close(); }
        Serial.printf("LOGSIZE sched=%ld old=%ld\n", s1, s2);
    } else if (strncmp(line, "LOGREADOLD", 10) == 0) {
        sscanf(line + 10, "%ld %ld", &a, &b);
        cmdReadChunk(LOG_PATH_OLD, a, b);
    } else if (strncmp(line, "LOGREAD", 7) == 0) {
        sscanf(line + 7, "%ld %ld", &a, &b);
        cmdReadChunk(LOG_PATH, a, b);
    } else if (strncmp(line, "LOGCLEAR", 8) == 0) {
        waitEPDReady();
        bool r1 = SD.remove(LOG_PATH);
        bool r2 = SD.remove(LOG_PATH_OLD);
        Serial.printf("LOGCLEAR sched=%d old=%d\n", r1 ? 1 : 0, r2 ? 1 : 0);
    } else if (strncmp(line, "LOGHELP", 7) == 0) {
        Serial.println("LOGHELP cmds: LOGSIZE | LOGREAD <off> <len> | LOGREADOLD <off> <len> | LOGCLEAR");
    }
}

// loop() から毎回呼ぶ。改行終端でコマンドを組み立てて処理する。
void pollSerialCommands() {
    static char line[80];
    static int li = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (li > 0) { line[li] = '\0'; handleLogCommand(line); li = 0; }
        } else if (li < (int)sizeof(line) - 1) {
            line[li++] = c;
        }
    }
}
