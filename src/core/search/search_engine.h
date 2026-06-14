// core/search: ファイル内検索・置換エンジン（PCRE2、pcre2-16/UTF対応）。
// design.md 3章 core/search「ファイル内検索・置換のエンジン（PCRE2、pcre2-16/UTF対応。後方参照・
// キャプチャ参照・Unicode文字クラス）。巨大/長行入力はワーカーで実行しキャンセル可」。要件5.4。
// sprint9 must（検索/正規表現置換/Unicode文字クラス/キャンセル可能）。
//
// 純ロジック（UI/WebView2/Win32 非依存）。PCRE2 は本ヘッダに出さず search_engine.cpp の実装詳細に
// 閉じる（公開 API は std::string=UTF-8 と Result<T> のみ。CLAUDE.md）。
//
// 文字列は UTF-8。内部で UTF-16 に変換して PCRE2(pcre2-16) で照合し、結果は UTF-8 バイト位置へ写し
// 戻す（pcre2-16/UTF で Unicode 文字クラスを正しく扱う。要件5.4）。
#pragma once

#include "core/search/cancel_token.h"
#include "core/search/search_types.h"
#include "util/result.h"

#include <string>
#include <string_view>

namespace pika::core::search
{

class SearchEngine
{
  public:
    explicit SearchEngine(SearchLimits limits = {}) : limits_(limits) {}

    // text 内で pattern に一致する全ヒットを列挙する（要件5.4「全ヒット・ヒット件数」）。
    // opts に従い 大文字小文字区別 / 単語単位 / 正規表現 を切り替える。
    // 正規表現が不正なら Result::err(InvalidArgument)（例外を投げない）。
    // 入力が SearchLimits 超過なら照合せず SearchResult{truncated=true}。
    // cancel が途中でセットされたら SearchResult{cancelled=true}（協調キャンセル）。
    pika::util::Result<SearchResult> find_all(std::string_view text, std::string_view pattern,
                                              const SearchOptions& opts,
                                              const CancelTokenPtr& cancel = nullptr) const;

    // text 内の全ヒットを replacement で置換した全文を返す（要件5.4「全置換」）。
    // opts.regex のとき replacement 内の $1 / ${12} / \1（後方参照=キャプチャ参照。要件5.4）を
    // 対応するキャプチャグループに展開する。$$ は '$'、$0 は全体一致。
    // 正規表現が不正なら Result::err(InvalidArgument)。入力過大は ReplaceResult{truncated=true}。
    // cancel セット時は ReplaceResult{cancelled=true}（途中結果は適用しない）。
    pika::util::Result<ReplaceResult> replace_all(std::string_view text, std::string_view pattern,
                                                  std::string_view replacement,
                                                  const SearchOptions& opts,
                                                  const CancelTokenPtr& cancel = nullptr) const;

    const SearchLimits& limits() const noexcept { return limits_; }

  private:
    SearchLimits limits_;
};

} // namespace pika::core::search
