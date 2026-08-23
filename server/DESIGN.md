# ホスト側サーバ 設計 (server/m5sched)

## 目的

M5Paper を「画面と鳴動だけ」の端末にし、落ちやすい処理 (TLS, 大きな ICS, RRULE, 状態保持, 通知) を
すべて Ubuntu ホストに置く。ホストが唯一の真実 (single source of truth)。端末は信用しない。

## モジュール

| ファイル | 役割 |
|---|---|
| `config.py` | `config.json` の読み込み。`/etc/m5sched` (`/etc/m5shed`) の旧 SD レイアウトを優先。旧キー (`ics_url`, `alarm_offset`, `midi_file`, `time_24h`...) を新キーへ写像。ESP32 のメモリ妥協だった `max_events/max_desc_bytes/min_free_heap/ics_poll_min` は無視 |
| `ics.py` | `IcsSource` (HTTPS 取得 + 直近成功コピーのディスクキャッシュ、`file://` 可)、`parse_sources` (icalendar + recurring_ical_events で窓内に展開)、`parse_alarm_marker` (`!` 文法、C++ 版と同一意味)、`trim_events` |
| `core.py` | `Scheduler` (テーブル・rev・アラーム状態機械・永続化・端末 API の実体)、`DeviceState` (端末の観測値・コマンドキュー・異常記録) |
| `memmon.py` | `MemMonitor` (ハートビートの heap/maxblock/psram を時系列保持、回帰で傾き算出、警告、予防再起動判断) |
| `notify.py` | ntfy (別スレッドで非同期送信) |
| `api.py` | `ThreadingHTTPServer` 上の REST。端末向け/人向けエンドポイント、MIDI キャッシュ、スクリーンショット → PNG |
| `dashboard.py` | 単一 HTML (外部アセットなし) |
| `__main__.py` | 起動・SIGTERM 処理 |

スレッド: `fetch` (ICS 取得ループ)、`tick` (1 秒毎の監視)、HTTP ワーカー (リクエスト毎)、`ntfy` (送信毎)。
共有状態は `Scheduler.lock` (RLock) と `DeviceState.lock` で保護。

## データフロー

```
ICS(HTTPS/file) ─fetch thread─▶ parse_sources ─▶ trim ─▶ _arm_alarms ─▶ events[] (rev++ if changed)
                                                                     └▶ data/events_cache.json
alarm_state{alarm_id → triggered/how/at/notified/pushed} ─▶ data/state.json
端末 ◀─GET /api/v1/events (NDJSON, rev, now, 表示設定)
端末 ─POST /api/v1/heartbeat─▶ DeviceState 更新, MemMonitor.add, 異常判定 ─▶ 応答 {now, rev, cmds[]}
端末 ─POST /api/v1/alarm/ack─▶ alarm_state[id].triggered=True, rev++
tick ─▶ 端末 offline 判定 / 鳴動時刻到来 → ntfy / 未 ACK → play 再送 / 放棄 → MISSED
```

## 識別子

- `event_id = sha1(UID|start_epoch)[:12]` — 同じ予定の同じ回は取得のたびに同じ ID
- `alarm_id = f"{event_id}-{offset:+d}"` (端末は `"%s-%+d"` で同じ文字列を作る)

## アラーム状態機械 (alarm_state[alarm_id])

```
(新規) ── at < now-grace ──▶ expired  (初回ロード or 開始+24h より古い)      ※鳴らさない
       └─ それ以外 ──────▶ armed
armed ── at <= now ──▶ due: notified=True, ntfy 送信
due   ── 端末 ACK ────▶ triggered (how="device:done|stale|failed")
due   ── 25s×n 未 ACK かつ端末 online ──▶ play コマンド push (最大 3 回)
due   ── 180s 未 ACK ──▶ triggered (how="missed-noack"|"missed-offline"), ntfy「鳴動失敗」
armed ── 予定時刻が変わった ──▶ 再武装 (triggered=False)
```
状態は 14 日経過で GC。`first_load` はキャッシュ復元時 False (再起動で過去アラームを expired 扱いし直さない)。

## 端末の検証 (Active Sensing)

ハートビート毎に:
- `last_seen` 更新。`tick` で `offline_after_sec` 超過 → offline (ntfy)。復帰で "back online"
- `uptime` 巻戻り → 再起動検知 (reset 理由付き ntfy)、MemMonitor のベースライン再設定
- `seq` 欠落数、`now` との時刻ずれ (>30s で異常)、`allowed_fw` 不一致、不明 alarm_id の ACK → `anomalies`
- `ev[]` (タッチ/スイッチ/画面/発火) と `log[]` を取り込み (`data/log/device.log`)

## コマンドチャネル

`DeviceState.cmd_queue` → ハートビート応答 `cmds[]` に最大 10 件を **ACK されるまで毎回** 同梱。
端末は `id <= last_cmd_id` を重複として ACK だけ返す。`cmd_seq` は `int(time.time())` から始め、
サーバ再起動をまたいでも単調増加。

## メモリ監視 (memmon)

- 5 秒毎のサンプルを 24 h 分保持、1 分毎に `data/log/mem.csv`
- 6 h 窓の最小二乗で heap / maxBlock の傾き (KB/h)。起動 5 分後を基準に「起動後変化」
- 警告 (起動毎に 1 回ずつ、ntfy): heap < 80KB, maxBlock < 48KB, 傾き < −2KB/h
- critical (heap < 60KB または maxBlock < 36KB) かつ 鳴動中でない かつ 次アラームまで 15 分以上 → `reboot` コマンド (予防再起動)。
  端末は `ESP_RST_SW` で再起動すると画面を消さずに復帰し、テーブルはホストから再取得する
- しきい値は `config.json` の `memory.*`

## 永続化・再起動耐性

| ファイル | 内容 | 用途 |
|---|---|---|
| `data/state.json` | alarm_state, rev | 鳴動済みの真実 |
| `data/events_cache.json` | 直近テーブル | サーバ再起動直後・ICS 全滅時も配信を継続 (`loaded` フラグが立つまで端末には rev=-1 を返し同期させない) |
| `cache/ics/srcN.ics` | 直近成功 ICS | 取得失敗時に使用 |
| `cache/midi/` | 外部 URL から取得した MIDI | `/api/v1/midi/<name>` |

## セキュリティ

LAN 限定 (ufw で 10.1.0.0/16 のみ 8765)。`api_token` を設定すると端末向けエンドポイントは `X-Token` 必須。
MIDI 名・スクリーンショット名は `^[A-Za-z0-9_.\-]{1,63}$` のみ。

## 設定の優先順位

`python -m m5sched [path]` の引数 > `/etc/m5sched/config.json` (`/etc/m5shed`) > `server/config.json`。
`DEFAULTS` (config.py) に全キーと既定値。
