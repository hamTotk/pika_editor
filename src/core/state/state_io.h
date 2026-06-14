// core/state: state.json の読み書き（version 付き・アトミック書き込み・未知 version 安全側）。
// design.md 7章「state.json」「version フィールド・未知versionは安全側・一時ファイル→rename」/ K2。
// 要件10.1（状態復元）・12.1（クラッシュ耐性）・要件10章受け入れ基準「未知versionは読み込まず
// 書き戻さず再生成もせず安全側」。
//
// 永続化はすべて util/atomic_file 経由のアトミック書き込み。index_io（snapshot 台帳）と同一の
// version 規約に従い、未知（kStateVersion より新しい）version は読み込まず・書き戻さず・再生成も
// せず安全側に倒す（旧版が新版状態を破壊しない。K2）。JSON は snapshot の json_lite を共用する
// （pika 自身が書いた固定スキーマ JSON のみを読む。依存を増やさない）。
#pragma once

#include "core/state/state_types.h"
#include "util/result.h"

#include <string>
#include <string_view>

namespace pika::core::state
{

// AppState を JSON 文字列へシリアライズする（version を必ず含める）。
std::string serialize_state(const AppState& state);

// state.json をパースして AppState を返す。
//   - パース不能（破損）: ErrorCode::Io（呼び出し側は既定状態で起動する）
//   - 未知 version（新しい）: ErrorCode::Unsupported（読み込まず安全側）
pika::util::Result<AppState> parse_state(std::string_view text);

// state_path から state.json を読み込む。存在しなければ空の state（version=現行）を返す
// （初回起動）。破損・未知 version はエラーを返す（parse_state と同じ分類）。
pika::util::Result<AppState> load_state(std::string_view state_path);

// AppState を state_path へアトミックに書き出す（一時ファイル→rename）。
pika::util::Result<void> save_state(std::string_view state_path, const AppState& state);

} // namespace pika::core::state
