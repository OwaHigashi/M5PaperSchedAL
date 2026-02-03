# M5Paper ICS Alarm with Unit Synth

M5Paper v1.1 用のカレンダーアラームアプリケーション。
ICSカレンダーから `!` マーカー付き予定を読み取り、指定時刻にUnit Synth経由でMIDIを再生します。

## 機能

- **ICSカレンダー取得**: HTTPS、Basic認証対応、自動更新（バックオフ付き）
- **簡潔なアラームマーカー**: タイトルに `!` を入れるだけでOK、詳細指定も可能
- **MIDI再生**: Unit Synth (SAM2695) 経由、SysEx完全対応
- **タッチUI**: E-Ink画面でのイベント一覧/詳細表示（日本語対応）
- **設定メニュー**: 全18項目をオンスクリーンキーボードで編集可能

## ハードウェア

- M5Paper v1.1
- M5Stack Unit Synth（Port B接続がデフォルト）
- microSDカード
- スピーカー/イヤホン

## SDカード構成

```
/
├── config.json          # 設定（なければデフォルト値で起動）
├── fonts/
│   └── ipaexg.ttf       # 日本語フォント（必須）
├── midi/
│   └── alarm.mid        # デフォルトMIDI
└── midi-dl/             # URLからDLしたMIDIのキャッシュ（自動作成）
```

## config.json

```json
{
  "wifi_ssid": "your_ssid",
  "wifi_pass": "your_password",
  "ics_url": "https://example.com/calendar.ics",
  "ics_user": "",
  "ics_pass": "",
  "midi_file": "/midi/alarm.mid",
  "midi_url": "",
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

| キー | デフォルト | 説明 |
|------|-----------|------|
| `ics_poll_min` | 30 | ICS更新頻度（分） 5/10/15/30/60 |
| `play_duration` | 0 | 鳴動時間（秒） 0=1曲 |
| `play_repeat` | 1 | 繰り返し回数 |

## アラームマーカー (!...!)

### 最も簡単な使い方

**タイトルに `!` を1つ入れるだけでアラームが有効になります。**

```
会議!
歯医者の予約!
!ミーティング
```

### 詳細な指定

`!` と `!` で囲んで修飾子を指定できます。

| 記号 | 意味 | 例 |
|------|------|-----|
| `>N` | N分前 | `!>15!` |
| `<N` | N分後 | `!<5!` |
| `+file` | URLからDL | `!+song.mid!` |
| `-file` | SDの/midi/から | `!-local.mid!` |
| `@N` | N秒間鳴動 | `!@15!` |
| `@` | 1曲再生 | `!@!` |
| `*N` | N回繰り返し | `!*3!` |

### 組み合わせ例

```
会議!                    → デフォルト設定でアラーム（最も簡単）
会議 !>10!               → 10分前にアラーム
!>10+song.mid@15*2!      → 10分前、URLからDL、15秒間、2回
!<5-chime.mid@*3!        → 5分後、SDから、1曲、3回
!@8!                     → デフォルト時間前、8秒間鳴動
!>0@20*5!                → 時刻ちょうど、20秒間、5回
```

### タイトル vs 説明

- **タイトル**: 単独の `!` でもアラーム有効（最も簡単）
- **説明**: `!...!` の完全形式が必要

`@` と `*` で指定した値は設定メニューの選択肢の制約を受けません（例: 8秒等も可能）。

## 設定メニュー（全18項目）

| 項目 | 操作 | 選択肢 |
|------|------|--------|
| WiFi SSID/Pass | キーボード | 文字入力 |
| ICS URL/User/Pass | キーボード | 文字入力 |
| MIDI File | ファイル選択 | /midi/ 内から選択 |
| MIDI URL | キーボード | ベースURL |
| MIDI Baud | 選択 | 31250/31520/38400 |
| Port | 選択 | A(G25)/B(G26)/C(G18) |
| Alarm Offset | トグル | 0〜60分（5分刻み） |
| Time Format | トグル | 24h / 12h |
| Text Display | トグル | 折り返し / 切り詰め |
| ICS Poll | トグル | 5/10/15/30/60分 |
| Play Duration | トグル | 1曲/5秒/10秒/15秒/20秒 |
| Play Repeat | トグル | 1/2/3/4/5回 |
| ICS Update | 実行 | 即時再取得 |
| Sound Test | 実行 | テスト再生 |
| Save & Exit | 実行 | 保存して戻る |

## 操作

| スイッチ | 一覧画面 | 設定画面 |
|----------|---------|---------|
| L（左） | 前日 / 長押し:設定 | 上 / 一番上で戻る |
| R（右） | 翌日 | 下 |
| P（中央） | 設定メニュー | 決定 |

## 旧バージョンからの移行

v2.x 以前の `%AL%` 形式から変更されました：

| 旧構文 | 新構文 |
|--------|--------|
| `%AL%` | `!` または `!!` |
| `%AL>10%` | `!>10!` |
| `%AL+song.mid@15*3%` | `!+song.mid@15*3!` |

## ビルド

**Arduino IDE**: M5Stackパッケージ 2.1.4、ボード「M5Paper」、ArduinoJsonライブラリ

**PlatformIO**: `pio run -t upload`

## ライセンス

MIT License
