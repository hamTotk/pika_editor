// core/search: 検索・置換の入力オプションと結果型。
// design.md 3章 core/search「ファイル内検索・置換のエンジン（PCRE2、pcre2-16/UTF対応。後方参照・
// キャプチャ参照・Unicode文字クラス）」。要件5.4「検索・置換」。sprint9。
//
// 本ヘッダは wx・WebView2・Win32・PCRE2 を一切含まない（純データ型。PCRE2 は search_engine.cpp の
// 実装詳細に閉じる。design.md 13章「自動単体テストの対象は core/・util」）。
//
// オフセット・長さは UTF-8 バイト基準（pika の文字列は std::string=UTF-8 に統一。CLAUDE.md）。
// 検索は内部で UTF-16 に変換して PCRE2(pcre2-16) で照合し、結果は UTF-8 バイト位置へ写し戻す。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pika::core::search
{

// 検索条件（要件5.4「大文字小文字区別・単語単位・正規表現」）。
struct SearchOptions
{
    // 大文字小文字を区別するか（false で区別しない＝PCRE2_CASELESS）。
    bool case_sensitive = true;
    // 単語単位（単語境界 \b で囲んで照合する）。リテラル・正規表現の双方に適用する。
    bool whole_word = false;
    // pattern を正規表現として解釈するか。false ならリテラル（メタ文字をエスケープして照合）。
    bool regex = false;
};

// 1 件のヒット。バイトオフセット（UTF-8・対象先頭からの位置）で持つ。
// groups は正規表現のキャプチャグループ（[0] は全体一致）。リテラル検索では [0] のみ。
struct Match
{
    std::size_t begin = 0; // 一致開始バイトオフセット（含む）
    std::size_t end = 0;   // 一致終了バイトオフセット（含まない）

    // 各キャプチャグループのバイト範囲（[0]=全体）。未参加グループは {npos, npos}。
    struct Group
    {
        std::size_t begin = 0;
        std::size_t end = 0;
        bool matched = false; // そのグループが一致に参加したか（省略可能グループ対策）
    };
    std::vector<Group> groups;
};

// 検索結果。全ヒット列挙とヒット件数（要件5.4「ヒット件数表示、全ヒットのハイライト」）。
struct SearchResult
{
    std::vector<Match> matches;

    // 入力が大きすぎる等で照合を開始せずフォールバックしたか（後述 SearchLimits 超過）。
    bool truncated = false;

    // 協調キャンセルで中断したか（true のとき matches は途中までを持ち得る）。
    bool cancelled = false;

    std::size_t count() const { return matches.size(); }
};

// 全置換の結果。置換後テキストと置換件数。
struct ReplaceResult
{
    std::string text;         // 置換適用後の全文（UTF-8）
    std::size_t replaced = 0; // 置換した件数
    bool truncated = false;   // 入力過大で置換を開始しなかった
    bool cancelled = false;   // 協調キャンセルで中断（text は元のまま or 途中まで適用しない）
};

// 検索・置換の上限（要件5.4 / 2.2 の段階制）。PCRE2 は途中中断できないため、入力サイズで開始前に
// 判定する（別スレッド中断に頼らない＝差分 DiffLimits と同じ思想。design.md 8章 I6）。
struct SearchLimits
{
    // 対象テキストの総バイト数の上限（既定 200MB。第2段階＝200MB 超は読み取り専用で置換不可）。
    std::size_t max_total_bytes = 200u * 1024u * 1024u;
    // 1 回の検索で列挙する最大ヒット件数（爆発的ヒットでメモリを食い潰さない安全弁）。
    std::size_t max_matches = 1'000'000u;
    // PCRE2 の 1 マッチあたりの計算量上限（自己 ReDoS＝破滅的バックトラック対策。sprint9 high）。
    // ユーザー入力の正規表現は `(a+)+$` のように指数的バックトラックを起こし得る。match_limit は
    // マッチエンジンの内部ステップ上限、depth_limit はバックトラック再帰の深さ上限。超過すると
    // pcre2_match が MATCHLIMIT/DEPTHLIMIT を返し、ヒット列挙を安全に打ち切る（ハングしない）。
    std::uint32_t match_limit = 10'000'000u;
    std::uint32_t depth_limit = 10'000u;
};

} // namespace pika::core::search
