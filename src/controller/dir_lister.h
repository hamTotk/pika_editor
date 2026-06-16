// controller/dir_lister: ディスク列挙結果 → workspace Entry への正規化（wx/FS 非依存）。
// design.md 5.1 手順4（ツリー列挙）/ 要件4.1（フォルダ先行・自然順・除外）/ spec.md sprint3 must
// （逐次追加列挙でフォルダを開ける）。
//
// 実ディスク列挙（ReadDirectoryW 相当）はプラットフォーム層（dir_enumerator・系統B）が回すが、
// 「列挙したフルパスを、ワークスペースルート起点の相対パス（'/' 区切り）へ正規化し、除外を適用し、
// build_tree が食える Entry へ落とす」写像は wx にも実 FS にも依存しない決定論ロジックである。
// これを純関数として切り出し gtest で観測する（系統A）。逐次列挙では各バッチを normalize_entries で
// Entry 化し UnreadSet/build_tree へ渡すことで、UI スレッドを長くブロックせず段階的に木を太らせる。
#pragma once

#include "core/workspace/workspace_model.h"

#include <string>
#include <string_view>
#include <vector>

namespace pika::controller
{

// 1 件の生の列挙結果（プラットフォーム層が実 FS から集める）。
// abs_path はワークスペースルート配下の絶対パス（'/' でも '\\' 区切りでも可）。
struct RawListEntry
{
    std::string abs_path;
    bool is_dir = false;
};

// 絶対パスをワークスペースルート起点の相対パス（'/' 区切り・先頭/末尾区切りなし）へ正規化する。
// - root と abs は大文字小文字を無視して照合する（Windows FS の非感性。要件3.2 と整合）。
// - 区切りは '/'・'\\' いずれも受け、出力は '/' に統一する。
// - abs が root 配下でなければ空文字を返す（呼び出し側は破棄＝木に含めない。../ 越境の遮断）。
// - abs == root のときは空文字（ルート自身は相対パスを持たない）。
std::string to_workspace_rel_path(std::string_view root, std::string_view abs);

// 生の列挙結果列を、除外適用済みの workspace::Entry 列へ正規化する純関数。
// - 各 raw を to_workspace_rel_path で相対化（root 配下でない/ルート自身は捨てる）。
// - core/workspace::is_excluded で除外（.git/node_modules 等）を適用する（ViewModel
// 側で再実装しない）。
// - 重複 rel_path は最初の 1 件だけ残す（同一バッチ内の重複を畳む。決定論）。
// build_tree(entries, exclude) がそのまま食える（build_tree も除外を適用するが、列挙段で落とせば
// 除外配下の逐次列挙自体を止められる＝軽い）。入力順は保持する（build_tree が自然順へ整列する）。
std::vector<core::workspace::Entry> normalize_entries(std::string_view root,
                                                      const std::vector<RawListEntry>& raw,
                                                      const std::vector<std::string>& exclude);

} // namespace pika::controller
