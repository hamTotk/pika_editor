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

} // namespace pika::app
