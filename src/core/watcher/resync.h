// core/watcher: バッファオーバーフロー再同期と監視不能環境の定期ポーリング。
// design.md 5.2「バッファオーバーフロー回復」「監視不能環境のフォールバック」。
// 要件7章・9章。sprint3 must「バッファオーバーフロー再同期」/ should「定期ポーリング」。
//
// `ReadDirectoryChangesW` が
// ERROR_NOTIFY_ENUM_DIR（バッファ溢れ）を返したら取りこぼしが起きるため、
// 監視ルートを全再列挙し、ベースライン（前回確定の relPath→meta）と突き合わせて差分を再構成する。
// プレスクリーンは mtime+サイズ、不一致のみハッシュ比較（design.md 9章・起動時未読判定と同じ）。
// ポーリングフォールバックは同じ突き合わせを定期実行で再利用する（mtime+サイズプレスクリーン共有）。
//
// 再列挙は実 FS を触る（gtest はテンポラリフォルダで検証する。design.md 13章 F6）。
#pragma once

#include "core/watcher/fs_event.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pika::core::watcher
{

// 監視対象ファイルの確定済みベースライン 1 件分のメタ。
struct BaselineEntry
{
    std::uint64_t size = 0;
    std::uint64_t mtime_ns = 0;
    std::uint64_t hash_lf = 0; // LF 正規化内容ハッシュ
};

// relPath（監視ルート相対・'/' 区切り）→ ベースラインメタ。
using BaselineMap = std::unordered_map<std::string, BaselineEntry>;

// 既定の除外ディレクトリ名（design.md・要件4章。再列挙でも監視と同じく除外する）。
// 完全一致のディレクトリ名で枝刈りする。
bool is_excluded_dir(std::string_view name);

// root を全再列挙し、baseline と突き合わせて再同期に必要な FsEvent 列を返す。
//   - baseline に無い実在ファイル        → Created
//   - baseline にあるが mtime/size 変化  → ハッシュ比較。異なれば Modified（同一なら何も出さない）
//   - baseline にあるが実在しない        → Removed
// プレスクリーン（mtime+size
// 一致）で済むものはハッシュを計算しない（取りこぼさず・無駄読みしない）。 戻り値は relPath
// 昇順（決定論）。本関数は baseline を書き換えない（呼び出し側が反映する）。
std::vector<FsEvent> resync(std::string_view root, const BaselineMap& baseline);

} // namespace pika::core::watcher
