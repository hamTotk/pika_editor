// controller/baseline_merge: ディスク由来ベースライン × index.json 永続ベースラインのマージ。
// design.md 5.1 手順4・9章 / 要件9.2。中心体験「外部変更→差分→確認済み」の土台＝起動時の
// ベースライン確立を担う（F-013）。wx・Win32・実 FS 非依存（gtest 対象）。
//
// build_baseline_from_disk が作る「開いた時点の現 size+mtime（hash 無し）」のベースラインへ、
// snapshot index.json の確認済み永続ベースライン（baselineHash 付き）を rel 単位で上書きする。
// これにより未確認ファイルはプレスクリーン任せ（クリーン）、確認済みファイルは確認時点の内容と
// 比較される（mtime が秒精度のため resync がハッシュ照合し、変化していれば未読化する）。
//
// controller はアプリ/コントローラ層なので下位の core::snapshot・core::watcher 双方に依存してよい。
#pragma once

#include "core/snapshot/snapshot_types.h"
#include "core/watcher/resync.h"

namespace pika::controller
{

// disk（build_baseline_from_disk の結果）へ index の確認済みベースラインを上書きマージする。
//   - index の各 entry のうち baselineHash が非空のものだけを対象にする。
//   - disk[entry.rel_path] = { size=baseline_size, mtime_ns=baseline_mtime, hash_lf=parse_hex } で
//     上書きする（baselineHash は xxh3_64_lf_hex の16進＝content_hash_lf と同一値。stoull
//     で復元）。
//   - 実在しない rel（disk に無い）はスキップする＝消えたファイルはここで足さない（resync が
//     Removed を出す責務）。不正/空 hex の entry もスキップする（取り繕わない）。
core::watcher::BaselineMap merge_index_into_baseline(core::watcher::BaselineMap disk,
                                                     const core::snapshot::SnapshotIndex& index);

} // namespace pika::controller
