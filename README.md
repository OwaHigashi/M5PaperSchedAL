# M5Paper ICS Alarm with Unit Synth

M5Paper v1.1 用のカレンダーアラームアプリケーション。
ICSカレンダーから `%AL%` マーカー付き予定を読み取り、指定時刻にUnit Synth経由でMIDIを再生します。

## 機能

- **ICSカレンダー取得**: HTTPS、Basic認証対応、自動更新（バックオフ付き）
- **アラームマーカー**: 時刻・MIDI・鳴動時間・繰り返しを柔軟に指定
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

## アラームマーカー %AL%

イベントのタイトルまたは説明に埋め込みます（大文字小文字不問）。

### 修飾子一覧

| 記号 | 意味 | 例 |
|------|------|-----|
| `>N` | N分前 | `%AL>15%` |
| `<N` | N分後 | `%AL<5%` |
| `+file` | URLからDL | `%AL+song.mid%` |
| `-file` | SDの/midi/から | `%AL-local.mid%` |
| `@N` | N秒間鳴動 | `%AL@15%` |
| `@` | 1曲再生 | `%AL@%` |
| `*N` | N回繰り返し | `%AL*3%` |

### 組み合わせ例

```
%AL>10+song.mid@15*2%   → 10分前、URLからDL、15秒間、2回
%AL<5-chime.mid@*3%     → 5分後、SDから、1曲、3回
%AL@8%                   → デフォルト時間前、8秒間鳴動
%AL>0@20*5%              → 時刻ちょうど、20秒間、5回
```

`@` と `*` で指定した値は設定メニューの選択肢の制約を受けません（例: 8秒等も可能）。

## 設定メニュー（全18項目）

| 項目 | 操作 | 選択肢 |
|------|------|--------|
| WiFi SSID/Pass | キーボード | 文字入力 |
| ICS URL/User/Pass | キーボード | 文字入力 |
| MIDI File | ファイル選択 | /midi/ 内から選択 |
| MIDI URL | キーボード | ベースURL |
| MIDI Baud | 選択 | 31250/9600/38400/57600/115200 |
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

## ビルド

**Arduino IDE**: M5Stackパッケージ 2.1.4、ボード「M5Paper」、ArduinoJsonライブラリ

**PlatformIO**: `pio run -t upload`

## ライセンス

MIT License
