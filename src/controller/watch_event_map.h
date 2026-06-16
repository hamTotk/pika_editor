// controller/watch_event_map: ReadDirectoryChangesW の生通知 → core/watcher RawEvent への写像。
// design.md 5.2（外部変更の監視）/ 要件7章 / spec.md sprint4
// must（FILE_NOTIFY_INFORMATION→RawEvent）。
//
// 監視スレッド（プラットフォーム層・src/app/watch_thread）は FILE_NOTIFY_INFORMATION を 1 件ずつ
// 取り出し、(a) Action コード（FILE_ACTION_ADDED 等）→ RawAction の写像、(b) UTF-16 の相対パスを
// UTF-8 へ変換し区切りを '/' に正規化、の 2 段で RawEvent を組み立てる。このうち (a) の写像と (b)
// の 区切り正規化は wx・Win32・実 FS に依存しない決定論ロジックなので、本モジュールへ切り出して
// gtest で 観測する（系統A）。UTF-16→UTF-8 変換そのものは Win32
// API（MultiByteToWideChar）が要るため監視 スレッド側に残す（本モジュールは UTF-8
// 文字列を受け取る）。
#pragma once

#include "core/watcher/fs_event.h"

#include <optional>
#include <string>

namespace pika::controller
{

// FILE_ACTION_* の生数値（windows.h を含めずに値で扱う。値は WinNT.h で固定の安定 ABI）。
//   1=FILE_ACTION_ADDED 2=REMOVED 3=MODIFIED 4=RENAMED_OLD_NAME 5=RENAMED_NEW_NAME
inline constexpr unsigned int kActionAdded = 1;
inline constexpr unsigned int kActionRemoved = 2;
inline constexpr unsigned int kActionModified = 3;
inline constexpr unsigned int kActionRenamedOld = 4;
inline constexpr unsigned int kActionRenamedNew = 5;

// FILE_ACTION_* の生数値 → RawAction。未知/想定外コードは std::nullopt（呼び出し側は破棄）。
// 未知コードを Modified 等へ既定流ししないのは、誤った種別が未読/削除判定を汚すのを避けるため。
std::optional<core::watcher::RawAction> map_watch_action(unsigned int action_code);

// 監視ルート相対パス（UTF-8）の区切りを '\\' → '/' に正規化し、先頭/末尾の余分な区切りを除く。
// ReadDirectoryChangesW は監視ルート相対の '\\' 区切りパスを返す。core/watcher / workspace は
// '/' 区切り・先頭区切りなしを前提とするため、ここで一元的に正規化する（区切り規約の単一定義点）。
std::string normalize_watch_rel_path(const std::string& raw_rel_utf8);

// action_code と UTF-8 相対パス・観測時刻から RawEvent を組み立てる純関数。
// action_code が未知、または正規化後パスが空なら std::nullopt（呼び出し側は WatcherCore
// へ投入しない）。
std::optional<core::watcher::RawEvent> make_raw_event(unsigned int action_code,
                                                      const std::string& rel_utf8,
                                                      core::watcher::TimeMs at);

} // namespace pika::controller
