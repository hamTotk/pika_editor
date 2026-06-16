// controller/restore_plan: 状態復元の組み立て（wx 非依存。sprint7 must）。
// spec.md 系統A「状態復元の組み立て」/ design.md 7章 K2（version・未知versionは安全側）・5.1
// （引数なし起動で完全復元）/ 要件10.1・10.2 / sprint7 must#1。
//
// core/state の AppState（window/tabs/activeTab/treeExpanded/modeByType/theme/recent）を入力に、
// UI（MainFrame・タブ・ツリー・モード切替・差分トグル・ペイン収納）の復元に必要な構造を決定論的に
// 再構成する純粋ロジック。core/state は触らず（コア完了済み）、controller 層で AppState
// から復元素材へ 写像する。GUI（系統B）はこの RestorePlan
// を機械的に消費してタブ/ツリーを組み立てる。
//
// 安全側設計（design 7章 K2）:
//   - 欠落フィールドは安全な既定値で補完（diff_on=false・tree_pane_collapsed=false）。
//   - 未知 version（kStateVersion
//   より新しい）は「読まない＝復元しない」（旧版が新版状態を破壊しない）。
//     呼び出し側（load_state）は未知 version を ErrorCode::Unsupported で弾くが、本層でも version
//     を 再確認し、未知なら復元を見送って（空の RestorePlan）安全側へ倒す（二重ガード）。
//
// 表示モードの正規化（ui-design 14章のモデル変更）:
//   旧 state.json は per-tab の mode
//   を「4モード（preview/split/source/diff）」の文字列で持つ。これを
//   本書で確定した「3モード（source/split/preview）＋差分トグル（ON/OFF・直交）」へ正規化する。文字列
//   "diff" は (mode=source, diff_on=true)
//   として復元する（差分はソース面に差分トグルが乗る最小構成）。
#pragma once

#include "controller/diff_mode_model.h"
#include "core/state/state_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pika::controller
{

// 1 タブの復元素材（AppState::TabState を UI 復元用に正規化したもの）。
// 表示モードは ViewMode（3モード）＋ diff_on（直交トグル）へ正規化済み。
struct TabRestore
{
    std::string path;                 // 開いていたファイルの絶対パス
    std::int64_t caret = 0;           // キャレット位置（文字オフセット）
    std::int64_t scroll = 0;          // ソースのスクロール位置（先頭行）
    std::int64_t preview_scroll = 0;  // プレビュー側スクロール位置
    ViewMode mode = ViewMode::Source; // 正規化後の表示モード（既定 Source）
    bool diff_on = false; // 差分トグル（旧 "diff" モードからの正規化を含む。既定 false）
};

// テーマの現在値（システム追従の解決結果。state.json.theme.current）。
enum class ThemeKind
{
    System, // 解決不能/未保持＝システム追従（安全な既定）
    Light,
    Dark,
};

// UI 復元用の構造（RestorePlan）。MainFrame はこれを見てウィンドウ/タブ/ツリーを組み立てる。
struct RestorePlan
{
    bool restorable = false; // 復元してよいか（未知 version・破損では false＝既定起動）

    // ウィンドウ位置・サイズ（要件10.1）。
    std::int64_t window_x = 0;
    std::int64_t window_y = 0;
    std::int64_t window_width = 0;
    std::int64_t window_height = 0;
    bool window_maximized = false;
    bool has_window_geometry = false; // 幅・高さが有効（>0）か（0 のとき OS 既定に委ねる）

    std::string last_workspace;             // 引数なし起動で復元するワークスペース（空＝なし）
    std::vector<TabRestore> tabs;           // 復元するタブ（順序保持）
    std::int64_t active_tab = -1;           // アクティブタブ index（範囲外/タブ無し時 -1 へ正規化）
    std::vector<std::string> tree_expanded; // 展開していたツリーノード（相対パス・順序保持）
    bool tree_pane_collapsed = false; // ツリーペイン収納（欠落時の安全な既定 false。ui-design 7章）

    ThemeKind theme = ThemeKind::System;     // テーマ現在値（解決不能は System）
    std::vector<std::string> recent_files;   // 最近使ったファイル（最大20件・順序保持）
    std::vector<std::string> recent_folders; // 最近使ったフォルダ（最大20件・順序保持）
};

// AppState から RestorePlan を再構成する純粋関数（同一入力で同一出力）。
//   - version > kStateVersion（未知）: restorable=false（復元しない＝安全側。K2）。
//   - per-tab mode 文字列を ViewMode＋diff_on へ正規化（"diff"→source+diff_on / 不明→source）。
//   - active_tab はタブ範囲へクランプ（範囲外・タブ無しは -1）。
//   - window は width/height>0 のときのみ has_window_geometry=true。
//   - theme.current 文字列を ThemeKind へ（"light"/"dark"/それ以外＝System）。
//   - recent は最大 kRecentLimit 件へクランプ（順序保持・先頭優先）。
RestorePlan build_restore_plan(const core::state::AppState& state);

// 表示モード文字列（"source"/"split"/"preview"/"diff"）→ (ViewMode, diff_on) の正規化（純粋）。
// 不明文字列は (Source, false) へ安全側フォールバック。"diff" は (Source, true)。
void normalize_mode(const std::string& mode, ViewMode& out_mode, bool& out_diff_on);

// テーマ文字列（"light"/"dark"/"system"/空/不明）→ ThemeKind（純粋）。不明は System。
ThemeKind normalize_theme(const std::string& theme_current);

} // namespace pika::controller
