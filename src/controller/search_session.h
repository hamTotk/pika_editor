// controller/search_session: 検索/置換ロジックの結線（wx 非依存。sprint7 must）。
// spec.md 系統A「検索/置換ロジックの結線」/ design.md 5.7・10章（検索/置換 UI）/ 要件5.4 /
// sprint7 must#3。
//
// core/search::SearchEngine（find_all/replace_all・PCRE2・協調キャンセル）を呼ぶ DocumentController
// 系のフローを wx 非依存ロジックとして実装する。検索パネルの描画は系統B（GUI）だが、検索の実行・
// 結果のカーソル遷移（次へ/前へ・折り返し）・置換適用は決定論で観測できるよう本層へ切り出す。
//
// SearchEngine は内容（UTF-8 テキスト）を受け取り Match 列（UTF-8
// バイトオフセット）を返す。本セッション はその Match
// 列に対し「現在のキャレット位置から次/前のヒットへ」をバイトオフセットで決定論的に解く （Scintilla
// の選択範囲移動は GUI が NavTarget のバイト範囲を見て行う）。巨大入力/複雑度上限/キャンセル は
// SearchEngine の
// SearchResult（truncated/truncate_reason/cancelled）をそのまま伝える（握り潰さない）。
#pragma once

#include "core/search/cancel_token.h"
#include "core/search/search_engine.h"
#include "core/search/search_types.h"
#include "util/result.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace pika::controller
{

// カーソル遷移の対象（GUI が選択範囲に使う UTF-8 バイト範囲＋このヒットが何件目か）。
struct NavTarget
{
    bool found = false;    // ヒットが 1 件以上あり遷移先が決まったか
    std::size_t begin = 0; // 遷移先ヒットの開始バイトオフセット
    std::size_t end = 0;   // 遷移先ヒットの終了バイトオフセット（含まない）
    std::size_t index = 0; // 0 始まりの何件目か（ステータス「i/総数」表示用）
    std::size_t total = 0; // 総ヒット件数
    bool wrapped = false;  // 末尾→先頭（または先頭→末尾）へ折り返したか
};

// SearchEngine を 1 つ束ねた検索セッション。検索実行と結果に対するカーソル遷移・置換適用を担う。
// SearchEngine 自体はステートレスのため、上限（SearchLimits）だけを保持する薄いラッパ。
class SearchSession
{
  public:
    explicit SearchSession(core::search::SearchLimits limits = {}) : engine_(limits) {}

    // text 内の全ヒットを列挙する（SearchEngine::find_all へ委譲。Result/truncated/cancelled
    // を透過）。
    pika::util::Result<core::search::SearchResult> find_all(
        std::string_view text, std::string_view pattern, const core::search::SearchOptions& opts,
        const core::search::CancelTokenPtr& cancel = nullptr) const;

    // text 内の全ヒットを replacement で置換する（SearchEngine::replace_all へ委譲）。
    pika::util::Result<core::search::ReplaceResult> replace_all(
        std::string_view text, std::string_view pattern, std::string_view replacement,
        const core::search::SearchOptions& opts,
        const core::search::CancelTokenPtr& cancel = nullptr) const;

    const core::search::SearchEngine& engine() const noexcept { return engine_; }

  private:
    core::search::SearchEngine engine_;
};

// 検索結果の Match 列に対し、キャレット位置 caret（バイトオフセット）から「次のヒット」へ遷移する
// 純粋関数（要件5.4「次を検索」）。caret 以降で最初に始まるヒットを選ぶ。無ければ先頭へ折り返す
// （wrapped=true）。ヒット 0 件なら found=false。Match は begin 昇順で渡される前提（find_all
// の出力順）。
NavTarget next_match(const core::search::SearchResult& result, std::size_t caret);

// 「前のヒット」へ遷移する純粋関数（要件5.4「前を検索」）。caret より前で最後に始まるヒットを選ぶ。
// 無ければ末尾へ折り返す（wrapped=true）。ヒット 0 件なら found=false。
NavTarget prev_match(const core::search::SearchResult& result, std::size_t caret);

} // namespace pika::controller
