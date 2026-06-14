// core/snapshot: index.json の読み書き（version 付き・アトミック書き込み・未知 version 安全側）。
// design.md 7章「index.json のエントリ」「version フィールド」「一時ファイル→rename」/ K2。
//
// 永続化はすべて util/atomic_file 経由のアトミック書き込み（クラッシュ耐性。要件12.1）。
// 未知（kIndexVersion より新しい）version の index は読み込まず・書き戻さず・再生成もせず安全側に
// 倒す（旧版が新版状態を破壊しないため。K2）。
#pragma once

#include "core/snapshot/snapshot_types.h"
#include "util/result.h"

#include <string>
#include <string_view>

namespace pika::core::snapshot
{

// SnapshotIndex を JSON 文字列へシリアライズする（version を必ず含める）。
std::string serialize_index(const SnapshotIndex& index);

// index.json をパースして SnapshotIndex を返す。
//   - パース不能（破損）: ErrorCode::Io（呼び出し側は objects 走査復元へ進む）
//   - 未知 version（新しい）: ErrorCode::Unsupported（読み込まず安全側）
pika::util::Result<SnapshotIndex> parse_index(std::string_view text);

// index_path から index.json を読み込む。存在しなければ空の index（version=現行）を返す
// （初回オープン）。破損・未知 version はエラーを返す（parse_index と同じ分類）。
pika::util::Result<SnapshotIndex> load_index(std::string_view index_path);

// SnapshotIndex を index_path へアトミックに書き出す。
pika::util::Result<void> save_index(std::string_view index_path, const SnapshotIndex& index);

} // namespace pika::core::snapshot
