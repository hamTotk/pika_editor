// core/watcher: ファイル監視イベントの型。
// design.md 3章 core/watcher「`FileWatcher`, `FsEvent`」/ 5.2。要件7章。
//
// プラットフォーム層（`ReadDirectoryChangesW`）が生成する生イベントを、UI 非依存のコアロジックが
// 合成・正規化した結果を表す。生イベント（RawEvent）は OS から届く 1 件 1 件、確定イベント
// （FsEvent）は合成・正規化後に WorkspaceController へ渡す 1 件を表す。
//
// 本ヘッダは wx・Win32 を一切含まない（純ロジックとして gtest で決定論検証できる。design.md
// 13章）。
#pragma once

#include <cstdint>
#include <string>

namespace pika::core::watcher
{

// 監視時刻はミリ秒の単調増加値（テストはこれを注入して決定論化する）。
// 実機では `GetTickCount64` 相当をプラットフォーム層が供給する。
using TimeMs = std::uint64_t;

// OS から届く生の変更通知 1 件。`ReadDirectoryChangesW` の
// FILE_NOTIFY_INFORMATION（Action + 相対パス）に対応する。
enum class RawAction
{
    Added,      // FILE_ACTION_ADDED
    Removed,    // FILE_ACTION_REMOVED
    Modified,   // FILE_ACTION_MODIFIED
    RenamedOld, // FILE_ACTION_RENAMED_OLD_NAME
    RenamedNew, // FILE_ACTION_RENAMED_NEW_NAME
};

struct RawEvent
{
    RawAction action = RawAction::Modified;
    std::string path; // 監視ルート相対パス（UTF-8・区切りは '/' に正規化済みを前提とする）
    TimeMs at = 0;    // OS がイベントを観測した時刻（ms）
};

// 合成・正規化後の確定イベント種別。WorkspaceController が未読・ベースライン更新に使う。
enum class FsEventKind
{
    Created,  // 新規作成（NEW 単独の rename を含む）
    Modified, // 内容変更（連続書き込みの合成・上書き rename を含む）
    Removed,  // 削除（OLD 単独の rename を含む）
    Renamed,  // old→new のペアが揃った移動/改名
};

struct FsEvent
{
    FsEventKind kind = FsEventKind::Modified;
    std::string path;     // 対象パス（Renamed のときは新パス）
    std::string old_path; // Renamed のときのみ意味を持つ（旧パス）
    TimeMs at = 0;        // 合成結果が確定した時刻（最後の生イベント時刻）
};

} // namespace pika::core::watcher
