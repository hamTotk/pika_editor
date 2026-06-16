// controller/settings_view: settings.toml 監視反映の純粋写像（wx 非依存。sprint7 must）。
// spec.md 系統A（settings 反映）/ design.md 2章（Settings/SettingsWatcher）・5.1（起動最序盤の同期
// 読み込み）/ 要件10.3 / sprint7 must#4。
//
// core/settings::load_settings の結果（LoadResult＝Settings＋parse_ok＋warnings）を UI
// 設定へ反映する 純粋写像。pika は settings.toml
// に書き戻さない（読み取り専用）。GUI（系統B）は本結果を見て Scintilla・
// プレビュー・テーマ等へ適用し、warnings は通知バー集約 ViewModel（SettingsError）へ流す。
//
// 反映方針（要件10.3）:
//   - parse_ok=false（保存途中の不完全 TOML
//   等）は「再適用しない＝直前の有効値を維持」する指示を返す
//     （既定値への全戻しでちらつかせない）。warnings があれば SettingsError 通知を 1
//     本立てる素材を返す。
//   - 個々の不正値は load_settings が既定へ丸めて warnings に積む。本層はその warnings 件数を UI
//   へ伝える。
#pragma once

#include "controller/diff_mode_model.h"
#include "controller/restore_plan.h"
#include "core/settings/settings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pika::controller
{

// UI へ反映する設定の写し（GUI が Scintilla/プレビュー/テーマへ機械的に適用する素材）。
// core::settings::Settings の enum を controller/UI 側の語彙（ViewMode・ThemeKind）へ写し替え、
// GUI が core の enum を直接知らなくても適用できるようにする（レイヤー依存を一方向に保つ）。
struct UiSettings
{
    // ファイルツリー除外（.git/node_modules 等。要件4.1）。
    std::vector<std::string> exclude;

    // 巨大ファイル段階制の閾値（要件5.4/9.2）。差分自動オフ・置換UI無効化の判定に使う。
    std::uint64_t big_file_stage1_bytes = 0;
    std::uint64_t big_file_stage2_bytes = 0;

    // エディタ（Scintilla 配線。要件5.1/11章）。
    std::uint64_t tab_width = 4;
    bool tab_inserts_spaces = false;
    bool show_whitespace = false;
    bool word_wrap = false;
    std::string font_family;
    std::uint64_t font_size = 11;

    // プレビュー（要件6章）。
    bool allow_remote_resources = false;
    bool feature_mermaid = true;
    bool feature_math = true;
    bool feature_code_highlight = true;

    // 初回既定モード（3モードへ正規化。ui-design 14章。Diff は差分トグル側へ）。
    ViewMode default_mode = ViewMode::Split;

    // テーマ希望値（ThemeKind へ写す。現在値の解決は state 側。ui-design 12章）。
    ThemeKind theme = ThemeKind::System;

    // ポーリング間隔（秒。監視不能環境のフォールバック。要件7.4）。
    std::uint64_t poll_interval_seconds = 5;
};

// settings 反映の結果（GUI が見る素材）。
struct SettingsApplyResult
{
    // UI へ反映してよいか（parse_ok=false なら false＝直前値維持）。
    bool apply = true;
    // apply=true のとき反映する設定の写し（false でも既定で埋まる）。
    UiSettings settings;
    // 不正値で既定フォールバックした項目数（warning_keys.size() と一致。後方互換のため残す）。
    std::size_t warning_count = 0;
    // 既定フォールバックした不正な設定キー名（LoadResult.warnings をそのまま透過）。
    // 通知バー集約（NotificationKind::SettingsError）が「どのキーが不正か」を提示する素材。
    std::vector<std::string> warning_keys;
    // TOML 構文破損（保存途中の不完全 TOML 等）。
    bool parse_failed = false;
};

// core::settings::Settings を UiSettings へ写す純粋写像（enum 語彙の写し替えを含む）。
UiSettings to_ui_settings(const core::settings::Settings& s);

// core::settings::ViewMode（4モード）→ controller::ViewMode（3モード）の正規化（純粋）。
// Diff は差分トグル側のため Source へ畳む（モード自体は 3 種。ui-design 14章）。
ViewMode to_view_mode(core::settings::ViewMode m);

// core::settings::Theme → ThemeKind の写し（純粋）。
ThemeKind to_theme_kind(core::settings::Theme t);

// core/settings::load_settings の LoadResult を UI 反映指示へ写す純粋関数。
//   - parse_ok=false: apply=false（直前の有効値を維持。ちらつかせない。要件10.3）。
//   - parse_ok=true : apply=true・settings を写し・warnings 件数を伝える（通知バー
//   SettingsError）。
SettingsApplyResult apply_settings(const core::settings::LoadResult& load);

} // namespace pika::controller
