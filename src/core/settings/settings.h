// core/settings: settings.toml の読み込み・検証・既定値フォールバック。
// design.md 2章 settings（`Settings`/`SettingsWatcher`）/ 5.1「起動最序盤の同期読み込み」/
// 要件10.3。
//
// settings.toml は **ユーザー編集専用＝pika は書き戻さない**（読み取り専用）。本モジュールは
// TOML テキストを受け取り、検証済みの値モデル（Settings）と警告（不正値→既定フォールバック）を
// 返すだけの UI 非依存ロジックである。実 FS の監視（SettingsWatcher）は core/watcher を直接使う
// 明示的例外（design.md 2章）だが、本 sprint では決定論検証できるパース/検証層に集中する。
//
// 検証方針（要件10.3）:
//   - 不正な値（型違い・範囲外）は既定値にフォールバックし、警告フラグを立てる（起動不能にしない）
//   - 保存途中の不完全な TOML でパース失敗時は、直前の有効な設定（previous）を維持する
//     （既定値への全戻しでちらつかせない）
#pragma once

#include "util/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pika::core::settings
{

// 表示モード（要件6.1 の4モード）。初回既定モードに用いる。
enum class ViewMode
{
    Preview, // プレビューのみ
    Split,   // 分割
    Source,  // ソースのみ
    Diff,    // 差分
};

// 新規ファイルの改行コード（要件5.1 / 10.3）。
enum class Newline
{
    Lf,   // "\n"
    Crlf, // "\r\n"
};

// 新規ファイルのエンコーディング（要件5.1 / 10.3）。pika が新規作成時に書き出す既定。
enum class NewFileEncoding
{
    Utf8,     // BOM なし UTF-8
    Utf8Bom,  // BOM 付き UTF-8
    ShiftJis, // Shift_JIS
};

// テーマ（要件11.3）。current の解決値は state.json 側。settings は希望値のみ持つ。
enum class Theme
{
    System, // システム追従
    Light,
    Dark,
};

// 検証済みの設定値（要件10.3 の設定項目）。不正値は既定に丸められた後の値が入る。
// 他コアモジュール（snapshot・workspace・render・watcher）が参照する閾値・トグルを集約する。
struct Settings
{
    // ファイルツリー（要件4.1）: 既定除外リスト（.git/node_modules）。設定で編集可能。
    std::vector<std::string> exclude = {".git", "node_modules"};

    // 巨大ファイル段階制（要件5.4 / 9.2）。第1段階=10MB、第2段階=200MB（バイト数）。
    std::uint64_t big_file_stage1_bytes = 10ull * 1024 * 1024;
    std::uint64_t big_file_stage2_bytes = 200ull * 1024 * 1024;

    // 行長ガード（要件5.4）。1 行がこの文字数を超えると一部機能を抑制する。
    std::uint64_t max_line_length = 100000;

    // レンダリング暴走ガードの閾値（要件6章 / sprint4）。入力サイズから開始前判定に用いる。
    std::uint64_t render_max_image_px = 60000000;   // 画像 6000万px
    std::uint64_t render_max_svg_px = 80000000;     // SVG 展開 8000万px 相当
    std::uint64_t render_max_html_elements = 50000; // HTML 要素数上限

    // リモートリソース許可（要件6章）。既定オフ（オプトイン）。
    bool allow_remote_resources = false;

    // 初回既定モード（要件6.1）。
    ViewMode default_mode = ViewMode::Split;

    // 新規ファイルのエンコーディング・改行（要件5.1 / 10.3）。
    NewFileEncoding new_file_encoding = NewFileEncoding::Utf8;
    Newline new_file_newline = Newline::Crlf;

    // 折返し既定（要件5.5 / 11章）。
    bool word_wrap = false;

    // タブ幅・Tab キー挙動・空白/タブ可視化（要件5.1 / 11章）。
    std::uint64_t tab_width = 4;
    bool tab_inserts_spaces = false;
    bool show_whitespace = false;

    // フォント（要件11章）。サイズは pt。
    std::string font_family = "Consolas";
    std::uint64_t font_size = 11;

    // テーマ（要件11.3）。
    Theme theme = Theme::System;

    // スナップショット容量上限（要件9.4）。既定500MB（バイト数）。
    std::uint64_t snapshot_capacity_bytes = 500ull * 1024 * 1024;

    // スナップショットのベースライン除外パターン＝機密ファイル（要件9.1）。
    std::vector<std::string> sensitive_patterns = {".env*", "*.env", "*.key", "*.pem", "*secret*"};

    // 起動時の全件ハッシュ照合（要件9.2 / 4.2）。既定オフ（既知の制限を承知のうえ高速パス）。
    bool unread_full_hash_on_startup = false;

    // 監視不能環境のポーリング間隔（秒。要件7.4 / 12.1）。
    std::uint64_t poll_interval_seconds = 5;

    // 機能トグル（要件6章 / 10.3）。Mermaid・数式・コードハイライト。
    bool feature_mermaid = true;
    bool feature_math = true;
    bool feature_code_highlight = true;
};

// load の結果。不正値があれば warnings に項目名（キー）を積む（UI は通知バーで警告表示）。
// parse_ok=false は構文破損（保存途中の不完全 TOML 等）。この場合 settings は呼び出し元が渡した
// previous をそのまま返す（直前の有効値維持。要件10.3）。
struct LoadResult
{
    Settings settings;
    // TOML 構文として読めたか（false=直前値維持）。
    bool parse_ok = true;
    // 既定にフォールバックした項目のキー（型違い・範囲外）。
    std::vector<std::string> warnings;
};

// 既定の Settings。
Settings default_settings();

// TOML テキストを読み取り専用で検証して Settings を返す。
// previous は「保存途中の不完全 TOML でパース失敗時に維持する直前の有効値」（要件10.3）。
// 構文破損時は previous を返し parse_ok=false。構文 OK でも個々の値が不正なら既定へフォールバック
// して warnings に積む（起動不能にしない）。例外は投げない（内部で捕捉して LoadResult 化）。
LoadResult load_settings(std::string_view toml_text, const Settings& previous);

// previous を既定にした薄いラッパ（初回読み込み用）。
LoadResult load_settings(std::string_view toml_text);

} // namespace pika::core::settings
