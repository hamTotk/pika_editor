// app/dir_enumerator: 実 FS の逐次ディレクトリ列挙（プラットフォーム層）。
// design.md 5.1 手順4「ツリー列挙は表示後に非同期」/ 要件4.1（逐次追加列挙でフォルダを開ける・
// UI をブロックしない）/ spec.md sprint3 must。
//
// std::filesystem で root 配下を 1 階層ずつ列挙し、各バッチを RawListEntry として
// コールバックへ渡す（呼び出し側は controller::normalize_entries で Entry 化し build_tree
// へ供給）。 シンボリックリンクは辿らず（循環回避。要件4.1）、列挙はパス文字列を UTF-8 で扱う
// （util::path_to_utf8/utf8_to_path 経由で CP_ACP を介さない）。実 FS 依存のため系統B/Cで検証する。
#pragma once

#include "controller/dir_lister.h"

#include <functional>
#include <string>
#include <vector>

namespace pika::app
{

// 1 ディレクトリ分の列挙結果を受け取るコールバック（逐次）。
// dir_rel は列挙したフォルダの相対パス（ルートは ""）。entries は直下の子のみ（フルパス）。
using OnDirListed =
    std::function<void(const std::string& dir_rel, std::vector<controller::RawListEntry> entries)>;

// root 直下の 1 階層を列挙して on_listed を呼ぶ（同期・1 階層分）。
// シンボリックリンク/ジャンクションは is_dir=true で返すが辿らない（呼び出し側が展開判断）。
// 失敗（権限なし・消失）は空バッチで呼ぶ（呼び出し側は縮退表示。本 sprint は最小配線）。
void list_directory(const std::string& root_abs, const std::string& dir_rel,
                    const OnDirListed& on_listed);

// root 配下を再帰的に歩いて全エントリ（フォルダ＋ファイル）を RawListEntry で返す（F-021）。
// ツリーが 1
// 階層しか出ない不具合の解消＝サブフォルダ内ファイルも木に含めるため、入れ子全体を一括列挙し
// 呼び出し側で normalize_entries→build_tree に渡す（build_tree が rel_path から入れ子を構築する）。
// 性能（軽い）:
//   - exclude（.git/node_modules
//   等）に一致したディレクトリには降りない（disable_recursion_pending）。
//     巨大ディレクトリの暴走を入口で断つ。判定は core/workspace::is_excluded と同じ規則に合わせる。
//   - シンボリックリンク/ジャンクションは辿らない（既定。循環回避）。
//   - max_nodes に達したら打ち切り
//   capped=true（深さ無制限・件数暴走の最終ガード。無言打ち切りにしない
//     ため呼び出し側がログを出す）。
std::vector<controller::RawListEntry> enumerate_tree(const std::string& root_abs,
                                                     const std::vector<std::string>& exclude,
                                                     std::size_t max_nodes, bool& capped);

} // namespace pika::app
