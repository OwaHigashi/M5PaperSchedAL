#pragma once
//==============================================================================
// UI カラー定義 (M5Paper 4bit グレースケール: 0=黒 ～ 15=白)
//
// 全画面のUI色をここで一括管理します。
// 値を変更して再コンパイルするだけで見た目を調整できます。
//==============================================================================

// ── 共通 ──
#define COL_BG                 0    // 画面背景（白）
#define COL_TEXT              15    // 標準テキスト（黒）

// ── 予定リスト画面 (ui_list) ──

// 日付ヘッダー（予定あり）
#define COL_DATE_BG           12    // 背景（濃いグレー帯）
#define COL_DATE_TEXT          0    // 文字（黒・太字で描画）

// 日付ヘッダー（予定なし＝今日）
#define COL_DATE_EMPTY_BG     12    // 背景
#define COL_DATE_EMPTY_TEXT    3    // 文字（ダークグレー・細字）
#define COL_NO_EVENT_TEXT      8    // 「予定なし」文字

// カーソル（選択行）
#define COL_CURSOR_BG          4    // 選択行の背景（0=黒 ～ 15=白）

// 変更行ハイライト（fetch後に表示内容が変わった行）
#define COL_CHANGED_BG         2    // 薄い灰色

// イベント行
#define COL_ROW_TEXT          15    // イベント行の文字

// アラームマーク(♪)バッジ
#define COL_ALARM_BADGE_BG    15    // バッジ背景（白）
#define COL_ALARM_BADGE_FG     0    // バッジ文字（黒）

// 「次の予定」区切り線
#define COL_NEXT_EVENT_LINE   15    // 線の色

// フッター（ボタン・ページ情報）
#define COL_BTN_BORDER        15    // ボタン枠
#define COL_BTN_TEXT          15    // ボタン文字
#define COL_FOOTER_TEXT       15    // ページ情報テキスト

// ヘッダー（時刻・WiFi状態）
#define COL_HEADER_TEXT       15    // ヘッダー文字

// 「予定がありません」（イベント0件時）
#define COL_EMPTY_MSG         15    // メッセージ文字

// ── 詳細画面 (ui_detail) ──
#define COL_DETAIL_BG          0    // 背景
#define COL_DETAIL_TEXT       15    // テキスト

// ── 設定画面 (ui_settings) ──
#define COL_SETTINGS_BG        0    // 背景
#define COL_SETTINGS_TEXT     15    // テキスト
#define COL_SETTINGS_CURSOR    4    // 選択項目の背景
#define COL_SETTINGS_BTN      15    // ボタン枠

// ── 再生画面 (ui_detail - drawPlaying) ──
#define COL_PLAYING_BG         0    // 背景
#define COL_PLAYING_TEXT      15    // テキスト
