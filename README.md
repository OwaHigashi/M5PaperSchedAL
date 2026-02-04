# M5Paper ICS Alarm

M5Paper と Unit Synth を使った、ICS カレンダー連携アラームシステム

## 概要

Google カレンダーなどの ICS 形式のカレンダーを取得し、予定に含まれるアラームマーカー（`!`）を検出して、指定時刻に MIDI 音源で通知します。ntfy.sh と連携することで、スマートフォンへのプッシュ通知も可能です。

## 特徴

- **ICS カレンダー連携**: Basic 認証対応、定期自動更新
- **柔軟なアラーム設定**: 予定のタイトルに `!` を付けるだけで簡単設定
- **MIDI 再生**: Unit Synth による高品質な音源再生（SysEx 対応）
- **タッチ UI**: e-Ink ディスプレイによる省電力な予定一覧表示
- **プッシュ通知**: ntfy.sh 連携で iPhone/Android に通知
- **完全オフライン**: WiFi 切断時も設定済みの予定でアラーム動作

## ハードウェア要件

| 部品 | 説明 |
|------|------|
| M5Paper v1.1 | メインデバイス |
| Unit Synth | MIDI 音源モジュール（Port B 接続推奨） |
| microSD カード | 設定ファイル、フォント、MIDI ファイル保存用 |

### 接続

Unit Synth を M5Paper の Port B（GPIO 26）に接続します。Port A/C も設定で選択可能です。

## ソフトウェア要件

### Arduino IDE ライブラリ

| ライブラリ | バージョン | 用途 |
|-----------|-----------|------|
| M5Stack (Board) | 2.1.4 | M5Paper サポート |
| M5EPD | 0.1.5 | e-Ink ディスプレイ制御 |
| ArduinoJson | 6.21.x 以降 | 設定ファイル読み書き |
| M5Unit-Synth | 1.0.x 以降 | MIDI ボーレート定義 |

### 組み込みライブラリ（別途インストール不要）

WiFi, WiFiClientSecure, HTTPClient, SD, time, base64（ESP32 コアに含まれる）

## インストール

### 1. ライブラリのインストール

Arduino IDE で以下を実行:
- ツール → ボード → ボードマネージャ → 「M5Stack」をインストール
- ツール → ライブラリを管理 → 「M5EPD」「ArduinoJson」「M5Unit-Synth」をインストール

### 2. SD カードの準備

以下のディレクトリ構造を作成:

```
SD カード/
├── config.json          # 設定ファイル
├── fonts/
│   └── ipaexg.ttf      # 日本語フォント（IPAexゴシック）
└── midi/
    └── alarm.mid       # デフォルトアラーム音
```

### 3. config.json の作成

```json
{
  "wifi_ssid": "your_wifi_ssid",
  "wifi_pass": "your_wifi_password",
  "ics_url": "https://example.com/calendar.ics",
  "ics_user": "",
  "ics_pass": "",
  "midi_file": "/midi/alarm.mid",
  "midi_url": "",
  "ntfy_topic": "",
  "midi_baud": 31250,
  "alarm_offset": 10,
  "port_select": 1,
  "time_24h": true,
  "text_wrap": false,
  "ics_poll_min": 30,
  "play_duration": 0,
  "play_repeat": 1
}
```

### 4. スケッチのアップロード

1. `M5PaperSchedAL.ino` と `SimpleMIDIPlayer.h` を同じフォルダに配置
2. Arduino IDE でスケッチを開く
3. ボード設定: M5Paper
4. アップロード

## 使い方

### アラームの設定方法

Google カレンダーなどで予定を作成し、**タイトルの末尾に `!` を付ける**だけです。

#### 基本的な書式

| 記述例 | 意味 |
|--------|------|
| `会議!` | デフォルト設定（10分前）でアラーム |
| `会議!!` | 同上（`!` と `!!` は同等） |
| `会議!-15!` | 15分前にアラーム |
| `会議!+5!` | 5分後にアラーム |

#### 詳細な書式

`!` と `!` の間にオプションを記述できます:

| 記号 | 意味 | 例 |
|------|------|-----|
| `-数字` | 数字分前 | `!-30!` = 30分前 |
| `+数字` | 数字分後 | `!+10!` = 10分後 |
| `<ファイル名` | SD カードの MIDI | `!<chime.mid!` |
| `>URL` | URL から MIDI をダウンロード | `!>https://...!` |
| `@秒数` | 再生時間（秒） | `!@15!` = 15秒 |
| `@` | 1曲再生（時間制限なし） | `!@!` |
| `*回数` | 繰り返し回数 | `!*3!` = 3回 |

#### 組み合わせ例

```
重要会議!-10<important.mid@20*2!
→ 10分前に important.mid を20秒×2回再生
```

### UI 操作

#### 予定一覧画面

| 操作 | 動作 |
|------|------|
| 予定をタップ | 詳細画面へ |
| 「<前日」 | 前日の予定へ |
| 「翌日>」 | 翌日の予定へ |
| 「今日」 | 今日の予定へジャンプ |
| 「設定」 | 設定画面へ |

**画面表示:**
- **日付ヘッダー**: 濃いグレー背景に白太字
- **♪マーク（反転）**: 未発火アラームあり
- **\*マーク**: 発火済みアラーム
- **アンダーライン**: 次の予定（直近の非終日予定）

#### 詳細画面

| 操作 | 動作 |
|------|------|
| 「戻る」 | 一覧へ戻る |
| 「テスト」 | この予定の設定でサウンドテスト |

#### アラーム再生中画面

| 操作 | 動作 |
|------|------|
| 画面タップ | アラーム停止 |

### 設定画面

本体の物理ボタン（L/R/P）または画面タッチで設定を変更できます。

| 設定項目 | 説明 | デフォルト |
|----------|------|-----------|
| WiFi SSID | WiFi アクセスポイント名 | - |
| WiFi Pass | WiFi パスワード | - |
| ICS URL | カレンダー ICS の URL | - |
| ICS User | Basic 認証ユーザー名（任意） | 空 |
| ICS Pass | Basic 認証パスワード（任意） | 空 |
| MIDI File | デフォルト MIDI ファイル | /midi/alarm.mid |
| MIDI URL | MIDI ダウンロード用ベース URL（任意） | 空 |
| MIDI Baud | MIDI ボーレート | 31250 |
| Port | Unit Synth 接続ポート | B |
| Alarm Offset | デフォルトアラーム分前 | 10 |
| Time Format | 12h/24h 表示切替 | 24h |
| Text Display | 折り返し/切り詰め | 切り詰め |
| ICS Poll | カレンダー更新間隔（分） | 30 |
| Play Duration | デフォルト再生時間 | 1曲 |
| Play Repeat | デフォルト繰り返し回数 | 1回 |
| Notify Topic | ntfy.sh トピック名 | 空 |
| Notify Test | 通知テスト実行 | - |
| ICS Update | カレンダー手動更新 | - |
| Sound Test | サウンドテスト実行 | - |
| Save & Exit | 保存して終了 | - |

## ntfy.sh 連携（プッシュ通知）

[ntfy.sh](https://ntfy.sh/) を使うと、アラーム発火時にスマートフォンへプッシュ通知を送信できます。

### 設定手順

1. スマートフォンに ntfy アプリをインストール（iOS/Android）
2. アプリでトピックを購読（例: `M5PaperAlarm-yourname`）
3. M5Paper の設定で `Notify Topic` に同じトピック名を入力
4. `Notify Test` で動作確認

### iPhone で通知が来ない場合

1. 設定 → ntfy → 通知 → すべて ON
2. ntfy アプリ → Settings → Instant delivery: ON
3. 集中モード（おやすみモード）を確認
4. アプリを再インストール

## ファイル構成

```
M5PaperSchedAL/
├── M5PaperSchedAL.ino   # メインスケッチ
├── SimpleMIDIPlayer.h   # MIDI プレイヤー（ヘッダオンリー）
└── README.md            # このファイル
```

## トラブルシューティング

### WiFi に接続できない

- SSID とパスワードを確認
- 2.4GHz ネットワークを使用（5GHz 非対応）

### MIDI が再生されない

- Port 設定（A/B/C）を確認
- Unit Synth の接続を確認
- MIDI ファイルが SD カードに存在するか確認

### カレンダーが取得できない

- ICS URL が正しいか確認
- Basic 認証が必要な場合は ICS User/Pass を設定
- WiFi 接続状態を確認

### アラームが発火しない

- 予定タイトルに `!` が含まれているか確認
- 予定の開始時刻が正しいか確認
- シリアルモニタでログを確認

## ライセンス

MIT License

## 謝辞

- [M5Stack](https://m5stack.com/) - M5Paper ハードウェア
- [IPAex フォント](https://moji.or.jp/ipafont/) - 日本語フォント
- [ntfy.sh](https://ntfy.sh/) - プッシュ通知サービス
