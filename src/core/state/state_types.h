// core/state: state.json（ウィンドウ・セッション状態）のメモリ表現。
// design.md 7章「state.json の主な内容」/ 要件10.1・10.2。UI 非依存・wx 非依存（gtest 対象）。
//
// 引数なし起動で完全復元するための窓・タブ・カーソル・スクロール・モード・ツリー展開・テーマ現在値・
// 最近20件を保持する。永続化（version・アトミック書き込み・未知 version 安全側）は state_io
// が担う。 本ヘッダは型のみを定義し、index.json（snapshot 台帳）とは別系統だが同じ version
// 規約に従う（K2）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pika::core::state
{

// ウィンドウ位置・サイズ・最大化（要件10.1）。
struct WindowState
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    bool maximized = false;
};

// 1 タブの復元情報（要件10.1）。mode はプレビュー表示モード（preview/split/source/diff）。
struct TabState
{
    std::string path;                // 開いていたファイルの絶対パス
    std::int64_t caret = 0;          // キャレット位置（文字オフセット）
    std::int64_t scroll = 0;         // ソースのスクロール位置（先頭行）
    std::string mode;                // 表示モード（"preview"/"split"/"source"/"diff"）
    std::int64_t preview_scroll = 0; // プレビュー側スクロール位置
};

// 「最近使った」項目（要件10.2）。ファイル・フォルダを各最大20件保持する。
struct RecentItems
{
    std::vector<std::string> files;
    std::vector<std::string> folders;
};

// state.json 全体のメモリ表現。
struct AppState
{
    int version = 1;
    WindowState window;
    std::string last_workspace; // 直前に開いていたワークスペース（引数なし起動で復元）
    std::vector<TabState> tabs;
    std::int64_t active_tab = -1;           // アクティブタブのインデックス（タブ無し時は -1）
    std::vector<std::string> tree_expanded; // 展開していたツリーノード（相対パス）
    // ファイル種別ごとの表現モード記憶（要件6.1。拡張子→モード）。挿入順を保つ k/v 列。
    std::vector<std::pair<std::string, std::string>> mode_by_type;
    std::string theme_current; // テーマ現在値（システム追従の解決結果。K7）
    RecentItems recent;
};

// state.json の現行スキーマ version（破壊的変更時のみ上げる単調増加。design.md 7章 K2）。
inline constexpr int kStateVersion = 1;

// 「最近使った」項目の保持上限（要件10.2「最近20件」）。
inline constexpr std::size_t kRecentLimit = 20;

} // namespace pika::core::state
