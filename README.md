# M5Paper ICS Alarm with Unit Synth (v029)

M5Paper v1.1 用のカレンダーアラームアプリケーション。
ICSカレンダーから予定を取得し、`!` マーカー付き予定を指定時刻にUnit Synth経由でMIDI再生します。

## 機能

- **ICSカレンダー取得**: HTTPS対応、Basic認証、ストリーミングパーサー、自動更新
- **アラームマーカー `!`**: タイトルに `!` を含む予定がアラーム対象。時刻・MIDI・鳴動時間・繰り返しを柔軟に指定可能
- **MIDI再生**: Unit Synth (SAM2695) 経由、SysEx完全対応、終了時GM Reset
- **タッチ＆スイッチUI**: E-Ink画面でイベント一覧・詳細表示・スクロール
- **ntfy通知**: アラーム発火時にスマートフォンへプッシュ通知
- **PSRAM活用**: イベントデータをPSRAM上に配置し、DRAMの断片化を完全排除
- **自動復旧**: ヒープ断片化検出時の自動リブート（アラーム保護付き）

## ハードウェア

| 部品 | 説明 |
|------|------|
| M5Paper v1.1 | ESP32 + 4.7" E-Ink + 8MB PSRAM |
| M5Stack Unit Synth (SAM2695) | MIDI音源 |
| Grove ケーブル | Port A/B/C いずれかに接続 |
| microSD カード | 設定・フォント・MIDIファイル格納 |

### ポート接続

| ポート | TX GPIO | 用途 |
|--------|---------|------|
| Port A | GPIO 25 | |
| Port B | GPIO 26 | デフォルト |
| Port C | GPIO 18 | |

## SDカードの準備

```
SD Root/
├── config.json          設定ファイル
├── fonts/
│   └── ipaexg.ttf       IPAexゴシック（必須）
├── midi/
│   └── alarm.mid        デフォルトアラーム音
└── midi-dl/             MIDI URL自動ダウンロード先（自動作成）
    ss1.pgm ...          スクリーンショット（自動作成）
```

### フォント

[IPAexゴシック](https://moji.or.jp/ipafont/) から `ipaexg.ttf` をダウンロードし `/fonts/` に配置。

## config.json

```jsonc
{
  "wifi_ssid": "your_ssid",        // WiFi SSID（2.4GHz）
  "wifi_pass": "your_password",     // WiFi パスワード
  "ics_url": "https://...",         // ICSカレンダーURL
  "ics_user": "",                   // Basic認証ユーザー（不要なら空）
  "ics_pass": "",                   // Basic認証パスワード（不要なら空）
  "ntfy_topic": "",                 // ntfy トピック名（空=通知無効）
  "midi_file": "/midi/alarm.mid",  // デフォルトMIDIファイル
  "midi_url": "",                   // MIDI DLのベースURL
  "midi_baud": 31250,              // MIDIボーレート
  "alarm_offset": 10,              // デフォルトアラーム（分前）
  "port_select": 1,                // 0=A, 1=B, 2=C
  "time_24h": true,                // 24時間制
  "text_wrap": false,              // テキスト折り返し
  "ics_poll_min": 5,               // カレンダー更新間隔（分、最小5）
  "play_duration": 0,              // 鳴動時間（秒、0=1曲再生）
  "play_repeat": 1,                // 繰り返し回数
  "max_events": 299,               // 最大イベント読み込み数（上限299）
  "max_desc_bytes": 3500,          // 説明文最大バイト数
  "min_free_heap": 40              // 最低空きヒープ（KB）
}
```

### パラメータ詳細

| パラメータ | デフォルト | 範囲 | 説明 |
|-----------|-----------|------|------|
| ics_poll_min | 5 | 5〜60 | 5未満は自動的に5に補正 |
| max_events | 299 | 10〜299 | MAX_EVENTS(300)-1が上限 |
| max_desc_bytes | 3500 | 100〜 | text[4000]の中にsummaryも含むため3500推奨 |
| min_free_heap | 40 | 20〜 | ICSフェッチ時のDRAM空き下限 |

## アラームマーカー

### 基本ルール

予定タイトルに `!` を含めるとアラーム対象になります。

```
14:00 会議!           → 10分前（デフォルト）にアラーム
14:00 会議!>5         → 5分前にアラーム
14:00 会議!>0         → 時刻ちょうどにアラーム
```

`!` はタイトルの任意の位置に配置できます。マーカー以降のパラメータ記号は表示から除去されます。

### パラメータ記号

| 記号 | 意味 | 例 |
|------|------|-----|
| `>N` | N分前にアラーム | `!>5` = 5分前 |
| `<N` | N分後にアラーム | `!<5` = 5分後 |
| `+filename` | MIDIファイル（URLからDL） | `!+song.mid` |
| `-filename` | MIDIファイル（SDカードから） | `!-chime.mid` |
| `@N` | N秒間鳴動（0=1曲） | `!@15` = 15秒 |
| `*N` | N回繰り返し | `!*3` = 3回 |

### 組み合わせ例

```
会議!>10+song.mid@15*2    → 10分前、URLからDL、15秒間、2回
打合せ!<5-chime.mid@*3    → 5分後、SDから、1曲、3回
集合!@8                    → デフォルト時間前、8秒間鳴動
発表!>0@20*5               → 時刻ちょうど、20秒間、5回
```

### マーカー検索優先順位

1. **SUMMARY（タイトル）** の `!` を最優先で検索
2. SUMMARY に `!` がなければ **DESCRIPTION（説明文）** を検索
3. 後方互換：旧 `%AL%` / `%AL>10%` 形式も引き続きサポート

### パラメータ優先順位

| 項目 | マーカー指定あり | マーカー指定なし |
|------|-----------------|-----------------|
| アラーム時刻 | `>N` / `<N` の値 | config の alarm_offset |
| MIDIファイル | `+file` / `-file` | config の midi_file |
| 鳴動時間 | `@N` | config の play_duration |
| 繰り返し | `*N` | config の play_repeat |

## 画面と操作

### 画面一覧

| 画面 | 説明 |
|------|------|
| イベント一覧 | 月別カレンダー表示。アラーム付きは `!` 表示 |
| イベント詳細 | SUMMARY + DESCRIPTION 全文表示（スクロール対応） |
| アラーム再生 | アラーム発火時の表示。時刻・タイトル・説明文 |
| 設定メニュー | 全20項目の設定変更 |
| キーボード | 文字入力（SSID/URL等） |
| MIDI選択 | /midi/ 内のファイル選択 |

### スイッチ操作

| スイッチ | 一覧画面 | 詳細画面 | 設定画面 | アラーム中 |
|----------|---------|---------|---------|-----------|
| L（左） | 前日 / 長押し:設定 | ↑スクロール | ↑ / 一番上で戻る | （なし） |
| R（右） | 翌日 | ↓スクロール | ↓ | （なし） |
| P（中央） | 設定メニュー | （なし） | 決定 | 停止 |

### タッチ操作

| 画面 | タッチ動作 |
|------|-----------|
| 全画面共通（左上） | スクリーンショット保存（PGM形式、SDカードへ連番保存） |
| 一覧画面 | イベントタップで詳細表示 |
| 詳細画面 | 任意タップで一覧に戻る |
| アラーム中 | 任意タップで停止 |

## スクリーンショット

任意の画面で左上隅（80×80ピクセル）をタッチすると、現在の画面内容をSDカードに保存します。

- 保存形式: PGM（Portable GrayMap）
- ファイル名: `/ss1.pgm`, `/ss2.pgm`, ... と連番
- 画面サイズ: 540×960ピクセル、グレースケール
- シリアルログ: `Screenshot saved: /ss3.pgm (540x960)`

PGMファイルはGIMP、IrfanView、ImageMagick等で開けます。PNGへの変換:
```bash
magick ss1.pgm ss1.png
```

## 設定メニュー（全20項目）

| 項目 | 操作 | 選択肢 |
|------|------|--------|
| WiFi SSID | キーボード | 文字入力 |
| WiFi Pass | キーボード | 文字入力 |
| ICS URL | キーボード | 文字入力 |
| ICS User | キーボード | 文字入力 |
| ICS Pass | キーボード | 文字入力 |
| MIDI File | ファイル選択 | /midi/ 内から |
| MIDI URL | キーボード | ベースURL |
| MIDI Baud | 選択 | 31250 / 31520 / 38400 |
| Port | 選択 | A(G25) / B(G26) / C(G18) |
| Alarm Offset | トグル | 0〜60分（5分刻み） |
| Time Format | トグル | 24h / 12h |
| Text Display | トグル | 折り返し / 切り詰め |
| ICS Poll | トグル | 1 / 5 / 10 / 15 / 30 / 60分 |
| Play Duration | トグル | 1曲 / 5 / 10 / 15 / 20秒 |
| Play Repeat | トグル | 1 / 2 / 3 / 4 / 5回 |
| Notify Topic | キーボード | ntfy トピック名 |
| Notify Test | 実行 | テスト通知送信 |
| ICS Update | 実行 | 即時再取得 |
| Sound Test | 実行 | テスト再生 |
| Save & Exit | 実行 | 保存して戻る |

## メモリ管理（v029）

### PSRAM活用

イベント配列（`EventItem[300]`）をPSRAM（8MB搭載、約4MB利用可能）に配置。
DRAMはWiFi/SSL/フォント等の動的処理専用となり、ヒープ断片化の問題を根本解決。

```
EventItem構造:
  text[4000]    summary \0 description \0 の結合バッファ
  midi_file[64] MIDIファイル名
  その他        時刻・フラグ等（約88byte）
  合計 ≒ 4KB/イベント × 300 = 約1.2MB（PSRAM上）
```

summaryとdescriptionは1つのバッファ`text[]`に格納されます。summaryは先頭から最初の`\0`まで、descriptionはその次の`\0`までです。summaryが短ければdescriptionに多くの領域が使え、柔軟に管理されます。

### 自動復旧メカニズム

| 条件 | 動作 |
|------|------|
| maxBlock < 45KB | 即リブート（アラーム保護付き） |
| ICSフェッチ連続失敗 | WiFiリセット→再フェッチ→失敗→リブート |
| アラーム5分以内 | リブート延期、アラーム完了後にリブート |

### アラーム保護

リブートが必要な場合でも、5分以内に発火予定のアラームがあればリブートを延期します。
アラームのMIDI再生が完了した後、自動的にリブートが実行されます。

## ntfy プッシュ通知

### セットアップ

1. スマートフォンに [ntfy](https://ntfy.sh/) アプリをインストール
2. アプリでトピックを購読（例: `M5PaperAlarm-yourname`）
3. M5Paper の設定で Notify Topic に同じトピック名を入力
4. Notify Test で動作確認

アラーム発火時に `HH:MM イベント名` の通知がスマートフォンに届きます。

## カレンダー更新

- 設定した間隔（ics_poll_min）で自動更新
- イベント0件の場合は30秒間隔で積極リトライ
- ICSストリーミングパーサーにより、ダウンロードとパースを同時処理
- 表示範囲：過去1日〜未来30日

## ファイル構成

```
M5PaperSchedAL/
├── M5PaperSchedAL.ino   メインスケッチ（setup/loop/復旧ロジック）
├── types.h              構造体・定数・列挙型定義
├── globals.h / .cpp     グローバル変数宣言・定義
├── config.cpp           config.json 読み書き
├── ics_parser.cpp       ICSストリーミングパーサー・フェッチ
├── input_handler.cpp    スイッチ・タッチ・アラーム発火処理
├── midi_player.cpp      MIDI再生制御
├── network.cpp          WiFi接続・ntfy通知・MIDIダウンロード
├── sd_utils.cpp         SD初期化・ヘルスチェック
├── ui_common.cpp        共通描画ユーティリティ
├── ui_list.cpp          イベント一覧画面
├── ui_detail.cpp        イベント詳細画面
├── ui_settings.cpp      設定メニュー画面
├── ui_keyboard.cpp      ソフトウェアキーボード
├── utf8_utils.cpp       UTF-8文字列処理
├── SimpleMIDIPlayer.h   SMF パーサー（ヘッダオンリー）
└── README.md            このファイル
```

## ビルド

### Arduino IDE

1. M5Stack ボードパッケージ 2.1.4 をインストール
2. ボードを「M5Paper」に設定
3. ArduinoJson ライブラリをインストール
4. M5PaperSchedAL.ino を開いてビルド・書き込み

## トラブルシューティング

### WiFi に接続できない

- SSID とパスワードを確認（2.4GHz のみ対応）
- シリアルモニタで接続状態を確認

### カレンダーが取得できない

- ICS URL が有効か確認（ブラウザでアクセスしてみる）
- Basic認証が必要なら ICS User/Pass を設定
- シリアルモニタで HTTP ステータスコードを確認

### MIDI が再生されない

- Port 設定（A/B/C）を確認
- Unit Synth の接続を確認
- Sound Test で動作テスト
- SD に MIDI ファイルが存在するか確認

### アラームが発火しない

- 予定タイトルに `!` が含まれているか確認
- シリアルモニタの ALARM CHECK ログで pending アラーム一覧を確認
- アラーム猶予期間: 予定時刻の10分以上過去は自動 triggered 扱い

### SD カードエラー

- SD カードを抜き差しして P ボタンを押す
- FAT32 フォーマットを確認

### シリアルモニタ（115200bps）ログの読み方

```
=== ALARM CHECK [02/14 12:30:00] ver.029 heap:220684 sd:OK ===
  [12] P1451.99 Draft Writing Meeting!
      event:02/18 23:00  alarm:02/18 22:50  off:10min  remain:377888s
=== events:76, pending:6, heap:220684, maxBlock:110580, WiFi:-43, fails:0 ===
```

- **heap**: DRAM空き容量
- **maxBlock**: 最大連続空きブロック（SSL接続に約45KB必要）
- **fails**: 連続フェッチ失敗回数（0が正常）
- **pending**: 未発火のアラーム数

## 仕様

| 項目 | 値 |
|------|-----|
| ディスプレイ | 540 x 960 e-Ink（M5Paper v1.1） |
| プロセッサ | ESP32（DRAM 520KB + PSRAM 8MB） |
| MIDI通信 | Serial2 TX（ハーフデュプレックス） |
| MIDI形式 | SMF Format 0/1、SysEx 対応 |
| 最大イベント数 | 300（PSRAM上、config で制限可能） |
| イベントバッファ | 4KB/イベント（summary + description 結合） |
| 日時表示 | 24時間制 / 12時間制 |
| タイムゾーン | JST (UTC+9) 固定 |
| NTPサーバー | pool.ntp.org, time.google.com |
| ICS更新間隔 | 1 / 5 / 10 / 15 / 30 / 60分 |
| アラーム猶予期間 | 600秒 |
| WiFi接続タイムアウト | 15秒 |
| HTTP読み取りタイムアウト | 15秒 |
| 自動表示更新 | 3分間無操作後、毎分チェック |
| フォント | IPAexゴシック (ipaexg.ttf) |

## ライセンス

MIT License

## 謝辞

- [M5Stack](https://m5stack.com/) — M5Paper ハードウェア
- [IPAexフォント](https://moji.or.jp/ipafont/) — 日本語フォント
- [ntfy.sh](https://ntfy.sh/) — プッシュ通知サービス
