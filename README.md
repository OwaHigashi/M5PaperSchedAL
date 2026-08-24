# M5Paper Schedule Display (v100 — thin client + host server)

M5Paper v1.1 をカレンダー表示・アラーム鳴動端末として使うシステムです。
v051 までは M5Paper 単体で ICS 取得(HTTPS)・RRULE 展開・アラーム管理をすべて行っていましたが、
ESP32 での 24 時間 SSL 運用はヒープ断片化・SSL バッファ・WiFi スタックの不安定さにより
「落ちる／時刻がずれる／予定が出ない／鳴らない」を根本的に解決できませんでした。

v100 では役割を分離しました。

| | 担当 |
|---|---|
| **Ubuntu ホスト** (`server/`) | ICS 取得 (HTTPS)・RRULE/RECURRENCE-ID/EXDATE 展開・`!` マーカー解釈・アラーム時刻と「鳴動済み」状態の管理・ntfy 通知・MIDI ファイル取得・端末の監視 (Active Sensing)・Web ダッシュボード |
| **M5Paper** (本リポジトリのファーム) | 画面表示・鳴動・タッチ/スイッチ操作の報告 **だけ**。SSL なし、SD カードなし |

端末とホストは LAN 内の **素の HTTP/1.1 (REST, JSON)** で通信します。端末側は `WiFiClient` を直接使う数百行の
クライアントだけで、`HTTPClient`/`WiFiClientSecure`/mbedTLS は一切リンクされません (Flash 1.08MB)。

## 構成

```
┌──────────── Ubuntu host (server/) ───────────────┐        ┌──── M5Paper ────┐
│ ICS(HTTPS) ──▶ parse/expand ──▶ events table(rev) │◀──GET──│ /api/v1/events   │ 全件同期 (NDJSON)
│ alarm state (data/state.json)   ntfy  MIDI cache │◀──POST─│ /api/v1/heartbeat│ 5秒毎: 生存・時刻・rev・コマンド
│ device supervision  dashboard  logs  screenshots │◀──POST─│ /api/v1/alarm/ack│ 鳴動完了
│ http://host:8765/                                 │        │ LittleFS: font/midi/config │
└───────────────────────────────────────────────────┘        └──────────────────┘
```

### 改善点 (v051 → v100)

| 症状 | 原因 (v051) | v100 |
|---|---|---|
| 落ちる・再起動ループ | SSL ハンドシェイクの 32KB 連続ブロック確保失敗、ヒープ断片化 | 端末から SSL を完全排除。ヒープ逼迫由来の再起動ロジックも撤廃 |
| 時刻がずれる | NTP 同期失敗・RTC ドリフト | 毎ハートビート (5秒) でホスト時刻と比較し、ずれが `max_skew_sec` (既定 2 秒) を超えたら即補正 |
| 予定が表示されない／更新されない | フェッチ失敗 → 全 X → 15 分再起動待ち | ホストが 1 分毎に取得。失敗時は直前の成功コピーを使い続ける。端末はテーブル版 (`rev`) が変わった時だけ全件取得 |
| 正しい時刻に鳴動しない・すっぽ抜け | 再フェッチで triggered 状態が消える／再武装される | 「鳴動済み」はホストが永続化 (state.json)。端末は鳴動完了を ACK し、届かなければ LittleFS に保持して再送。ホストは未 ACK を検知して play コマンドを再送、それでも駄目なら ntfy で「鳴動失敗」を通知 |
| 取り込み期間・件数の妥協 | 2 週間・max_events 99・desc 500B | 過去 1 日〜先 60 日 (設定可)、299 件、desc 3500B。ホスト側で 1.3MB の ICS も問題なし |
| SD カード劣化・SPI 競合 | SD と EPD が SPI を共有 | SD 廃止。フォント/設定/MIDI は内蔵フラッシュ (LittleFS 12.9MB) |

### 端末を信用しない設計 (Active Sensing)

端末は 5 秒毎に `/api/v1/heartbeat` で自己申告します (FW 版・uptime・heap・電池・RSSI・画面状態・テーブル版・
鳴動中アラーム・未送 ACK 数・温度・操作イベント・ログ)。ホストは次を検証・記録します。

- 60 秒途絶 → **offline** (ntfy 通知、復帰時も通知)
- uptime が巻き戻った → **再起動検知** (reset 理由付きで通知)
- 時刻ずれ・ハートビート seq の欠落・想定外 FW・不明アラームの ACK → 異常として記録
- 鳴動時刻を過ぎて 25 秒 ACK がない → `play` コマンドを明示送信 (最大 3 回)。180 秒で「鳴動失敗」確定 + ntfy
- ホストは鳴動時刻になった時点で端末と無関係に ntfy を送る (端末が死んでいても予定は消えない)

端末 → ホストの **操作イベント** (タッチ座標・スイッチ・詳細表示した予定 ID・アラーム発火) はハートビートに同梱され、
ダッシュボードで見られます。ホスト → 端末の **コマンド** はハートビート応答で配られ、端末は ACK を返します。

| コマンド | 動作 |
|---|---|
| `refresh` | 全件再同期して再描画 |
| `redraw` | 再描画 (GC16) |
| `screenshot` | フレームバッファをホストへ送信 → `data/screenshots/*.png` |
| `play` `{midi,duration,repeat,alarm_id?}` | 鳴動 (サウンドテスト／アラーム再送) |
| `stop` | 鳴動停止 |
| `message` `{text,hold_ms}` | 画面にメッセージ表示 |
| `show` `{event_id}` | 指定予定の詳細を表示 |
| `reboot` | 再起動 |
| `config` `{time_24h,text_wrap}` | 表示設定変更 |

## ホスト側セットアップ (Ubuntu)

FHS に沿って 3 箇所に分かれる (`server/` はリポジトリ内では `/opt/m5sched` へのシンボリックリンク):

```
/opt/m5sched/          プログラム (m5sched/ パッケージ, .venv, requirements.txt, 同梱 midi/)
/etc/m5sched/          設定 (config.json, certs/, fonts/, midi/) — config.json は ICS の private URL を含むので 600
/var/lib/m5sched/      可変データ (data/: state, events_cache, log/, screenshots/  cache/: ics, midi)
```

```bash
sudo mkdir -p /opt/m5sched /etc/m5sched /var/lib/m5sched && sudo chown $USER /opt/m5sched /etc/m5sched /var/lib/m5sched
rsync -a server/ /opt/m5sched/ --exclude .venv
python3 -m venv /opt/m5sched/.venv && /opt/m5sched/.venv/bin/pip install -r /opt/m5sched/requirements.txt
cp server/config.json.example /etc/m5sched/config.json   # ICS URL / ntfy などを記入し、
#   "data_dir": "/var/lib/m5sched/data", "cache_dir": "/var/lib/m5sched/cache",
#   "ca_file": "/etc/m5sched/certs/ca-bundle.pem", "midi_dir": "/etc/m5sched/midi" を絶対パスで指定
/opt/m5sched/.venv/bin/python -m m5sched              # 手動起動 (http://localhost:8765/)
```

systemd (このホストでは導入済み):

```bash
sudo cp /opt/m5sched/m5sched.service /etc/systemd/system/
sudo systemctl enable --now m5sched
sudo ufw allow from 10.1.0.0/16 to any port 8765 proto tcp   # LAN のみ許可
journalctl -u m5sched -f
```

### /etc/m5sched/config.json

| キー | 既定 | 説明 |
|---|---|---|
| `ics_urls` | `[]` | `{"url","user","pass"}` の配列。`file://` も可 (テスト用) |
| `ics_poll_sec` | 60 | ICS 取得間隔 |
| `window_past_days` / `window_future_days` | 1 / 60 | 取り込み範囲 |
| `max_events` / `max_desc_bytes` | 299 / 3500 | 端末テーブルに収める上限 |
| `alarm_offset_default` `play_duration` `play_repeat` `midi_default` `midi_url` | | 旧 config.json と同じ意味 (ホスト側に移動) |
| `ntfy_server` `ntfy_topic` `ntfy_on_*` | | 通知 |
| `device.heartbeat_sec` | 5 | Active Sensing 間隔 (端末へ配布) |
| `device.offline_after_sec` | 60 | オフライン判定 |
| `device.full_sync_sec` | 600 | rev が同じでも最低この間隔で全件再取得 |
| `device.time_24h` `device.text_wrap` | | 端末の表示設定 (ホストから配布) |
| `device.max_skew_sec` | 2 | 端末がこの秒数以上ずれたら時刻補正 |
| `ca_file` / `ics_verify_tls` | | 証明書チェーンが不完全なサーバ用 (例: `/etc/m5sched/certs/ca-bundle.pem`) |
| `api_token` | "" | 設定すると端末は `X-Token` ヘッダを要求される |

### 設定ディレクトリ `/etc/m5sched`

サーバは `/etc/m5sched/config.json` を読み (旧 SD カード形式の `config.json` もそのまま受け付ける)、
`midi/` の MIDI を端末へ配信します。旧 SD の `config.json` は `config.json.sd-legacy` として保存してある。端末側は `tools/make_data.sh` が同ディレクトリから `data/` を生成して
フラッシュします。旧 `max_events / max_desc_bytes / min_free_heap / ics_poll_min` は ESP32 のメモリ妥協だったため無視されます。

### メモリリーク監視

ハートビートの `heap / maxblock / psram` をホストが時系列保持し、ダッシュボードにグラフ・傾き (KB/h)・
起動後変化・予防再起動までの予測を表示します (`/var/lib/m5sched/data/log/mem.csv` に 1 分毎)。しきい値は `memory.*`:
heap < 80KB / maxBlock < 48KB / 減少 2KB/h 超 → 警告 (ntfy)、heap < 60KB or maxBlock < 36KB → 鳴動中でなく
次アラームまで 15 分以上あるときに予防再起動コマンド。設計の詳細は `server/DESIGN.md`。

### データ

```
/var/lib/m5sched/data/state.json          アラーム「鳴動済み」状態 (永続)
/var/lib/m5sched/data/events_cache.json   最後のテーブル (再起動・ICS 障害時もこれで動く)
/var/lib/m5sched/data/log/alarm.log       due / ACK / MISSED / 端末 online-offline
/var/lib/m5sched/data/log/device.log      端末から転送されたログ (旧 SD ログの代替)
/var/lib/m5sched/data/screenshots/        端末スクリーンショット (PNG)
/var/lib/m5sched/cache/ics/, cache/midi/  ICS 直近成功コピー、MIDI キャッシュ
/var/lib/m5sched/sd-legacy/               旧 SD カードにあった log/ screenshots/ cache/ (退避)
```

### ダッシュボード `http://<host>:8765/`

端末状態 (online/offline, 時刻ずれ, heap, 電池, RSSI, 再起動回数, 異常検知)、ICS 取得状況、予定一覧と
アラーム状態、コマンド送信 (再同期・再描画・スクリーンショット・サウンドテスト・停止・再起動・メッセージ)。

### REST API

| | |
|---|---|
| `GET /api/v1/events` | NDJSON: 1 行目 header (`rev,now,tz,count,表示設定`), 以降 1 イベント 1 行, 末尾 `{"type":"end"}` |
| `POST /api/v1/heartbeat` | 端末状態 → `{now,tz,rev,hb_sec,full_sync_sec,max_skew,cmds[],next_alarm}` |
| `POST /api/v1/alarm/ack` | `{alarm_id,result}` |
| `POST /api/v1/cmd/ack` | `{id,ok,info}` |
| `GET /api/v1/midi/<name>` | MIDI バイト列 (なければ `midi_url` から取得してキャッシュ) |
| `POST /api/v1/screenshot` | 4bpp フレームバッファ (ヘッダ `X-Width`/`X-Height`) |
| `GET /api/v1/status` `GET /api/v1/list` `POST /api/v1/cmd` `POST /api/v1/refresh` | ダッシュボード用 |

アラーム ID は `"<event_id>-<+offset>"` (例 `4ecf6462d6e4-+10`)。event_id は UID と開始時刻から作る安定ハッシュです。

## 端末側セットアップ (PlatformIO, Linux)

このホストには導入済み (`~/.platformio-venv`, `/usr/local/bin/pio`, udev ルール `/dev/m5paper`)。

```bash
pio run                 # ビルド
pio run -t upload       # ファーム書込 (/dev/m5paper)
pio run -t uploadfs     # LittleFS 書込 (data/ → フォント・MIDI・config)
pio device monitor      # シリアル (115200)
```

### 内蔵フラッシュ (LittleFS) の内容 — `data/`

```
data/
├── config.json          端末ローカル設定 (WiFi, サーバ host/port, MIDI ポート) ※git 管理外
├── config.json.example
├── fonts/ipaexg.ttf     IPAex ゴシック ※git 管理外 (6MB)。sdcard/fonts からコピー
└── midi/alarm.mid       ローカル既定 MIDI
```

パーティション (`partitions_m5paper.csv`): app 3MB + LittleFS 12.9MB。
`data/` を変更したら `pio run -t uploadfs`。実行時生成物: `/midi-dl/` (ホストから取得した MIDI), `/pending_acks.txt`。

### data/config.json

```json
{
  "wifi_ssid": "...", "wifi_pass": "...",
  "server_host": "10.1.1.2", "server_port": 8765, "api_token": "",
  "midi_file": "/midi/alarm.mid", "midi_baud": 31250, "port_select": 1
}
```

`server_host` 未設定時は `platformio.ini` の `DEFAULT_SERVER_HOST` が使われます。設定メニュー (P ボタン) からも変更できます。

### シリアルコマンド

`STATUS` / `SYNC` / `HB` / `REBOOT` (改行終端)。

## アラームマーカー `!` (仕様は v051 と同じ。判定はホスト側)

タイトルまたは説明文に `!` を含めるとアラーム対象。

| 記法 | 意味 |
|---|---|
| `会議!` / `!朝礼` | 既定オフセット (10 分前) |
| `!-10!` `!+5!` `!-0!` | 10 分前 / 5 分後 / 定刻 |
| `!-25,-15,-5!` | 複数 (最大 6) |
| `>song.mid` / `<chime.mid` | MIDI (ホスト経由で URL から取得 / 端末 `/midi/` 内) |
| `@N` | N 秒鳴動 (0 = 1 曲) |
| `*N` | N 回繰り返し |

全角 `！－１０！` も可。RRULE は `recurring_ical_events` により DAILY/WEEKLY/MONTHLY/YEARLY, INTERVAL, BYDAY,
BYMONTHDAY, COUNT, UNTIL, EXDATE, **RECURRENCE-ID** (個別回の変更), TZID をすべて正しく扱います。

### 鳴動状態のルール

- 初回起動時に既に過去 (10 分以上前) のアラームは expired (鳴らさない)
- 後から `!` を付けた予定は開始時刻 + 24 時間まで鳴らす (v051 と同じ)
- 予定の時刻が変わると再武装
- 鳴動済みは `*`、未鳴動は `♪` で一覧に表示

## 画面と操作

| 画面 | スイッチ | タッチ |
|---|---|---|
| 一覧 | L/R: 選択移動, P: 設定 | 行: 詳細, 前日/翌日/今日/詳細ボタン, 左上: スクリーンショット送信 |
| 詳細 | L/R: スクロール, P: 戻る | 任意: 戻る (30 秒で自動復帰) |
| 鳴動中 | — | 任意: 停止 |
| 設定 | L/R: 移動, P: 決定 | 項目タップ |

ヘッダー右: 最終同期時刻, ` fchNX` (ホスト側 ICS N 本目失敗), ` !W` WiFi 断, ` !H` ホスト不達, ` !T` 時刻未設定。
右上 ●: ホスト接続中は 5 秒で明滅、不達時は中空 ○ のまま。

設定メニュー: ホストと再同期 / Server Host / Server Port / WiFi SSID / WiFi Pass / MIDI File / MIDI Baud / Port /
Sound Test / スクリーンショット送信 / Save & Exit。

## ホスト不達時の端末の振る舞い

- 手元のテーブルで時刻通りに鳴動する (ACK は保持して後で再送)
- 10 分不達で WiFi を張り直し、2 時間不達で再起動
- 時刻はホストから受け取った最後の値で進む (RTC)。ホスト復帰時に補正

## ファイル構成

```
M5PaperSchedAL.ino   setup/loop (同期・ハートビート・自動更新)
types.h globals.h/.cpp
config.cpp           LittleFS /config.json
fs_utils.cpp         LittleFS 初期化・MIDI 一覧
network.cpp          WiFi・素の HTTP クライアント・MIDI 取得・スクリーンショット送信
sync_client.cpp      全件同期 (NDJSON)・ハートビート・コマンド・時刻合わせ・ACK 再送
logger.cpp           ログ/操作イベントのキュー (ハートビートで転送)
midi_player.cpp      MIDI 再生
input_handler.cpp    スイッチ/タッチ/アラーム発火
ui_*.cpp             画面
SimpleMIDIPlayer.h   SMF パーサ (LittleFS)
partitions_m5paper.csv
data/                LittleFS イメージ元
server/              ホスト側 (Python, 標準ライブラリ http.server + icalendar/recurring-ical-events/requests)
old/                 v051 の端末内 ICS パーサ等 (参考。ビルド対象外)
sdcard/              旧 SD カード内容 (フォントの取得元として残置)
```

## ハードウェア

| 部品 | |
|---|---|
| M5Paper v1.1 | ESP32 + 4.7" E-Ink + 8MB PSRAM + 16MB Flash |
| M5Stack Unit Synth (SAM2695) | Port A(G25)/B(G26, 既定)/C(G18) |

## 履歴

- **v100**: thin client 化。ICS/SSL/アラーム状態/ntfy をホスト (`server/`) へ移動。SD 廃止 → LittleFS。
  Active Sensing、コマンド/操作イベントの双方向通信、ホスト時刻同期、ACK 再送、ダッシュボード。
  取り込み範囲 2 週間 → 60 日、max_events 99 → 299、desc 500 → 3500B。
- v051 以前: 端末単体構成 (old/ と git 履歴を参照)
