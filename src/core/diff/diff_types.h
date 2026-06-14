// core/diff: 行差分・行内強調の結果型と入力オプション。
// design.md 3章 core/diff「`DiffEngine`, `DiffResult`」/ 8章「差分・既読の設計」。要件8章。
//
// 中心体験（spec.md「中心体験」4）の「前回確認時点からの累積差分を赤/緑で確認」の決定論部分。
// ベースライン内容（前回確認時点）と現在内容を入力に、LF 正規化照合の行差分と行内強調を返す。
//
// 本ヘッダは wx・WebView2・Win32 を一切含まない（純ロジックとして gtest で決定論検証できる。
// design.md 13章「自動単体テストの対象は core/・util」）。表示色は持たず、色非依存の記号
// （+/-）クラスのみを出力する（要件8.4 / design.md 8章 J5「色だけに依存せず判別できる」）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pika::core::diff
{

// 1 行の差分種別。色ではなく種別で持ち、UI 側が色＋記号（+/-）で描画する。
enum class LineOp
{
    Context, // 変更なし（両側に存在し内容一致）。記号は ' '（空白）
    Add,     // 追加行（現在のみに存在）。記号は '+'
    Delete,  // 削除行（ベースラインのみに存在）。記号は '-'
};

// 行内強調の 1 区間（変更語/変更文字の範囲）。
// バイトオフセット（UTF-8 の先頭からのバイト位置）で持ち、UI 側が原文に重ねて強調する。
// 文字単位フォールバック時も UTF-8 コードポイント境界を跨がない範囲のみを返す。
struct InlineSpan
{
    std::size_t begin = 0; // 強調開始バイトオフセット（行内・含む）
    std::size_t end = 0;   // 強調終了バイトオフセット（行内・含まない）
};

// 差分結果の 1 行。
// text は原文（LF 正規化前の改行は行分割で既に除去済み。表示は原文準拠＝design.md 8章）。
// op が色非依存の記号クラスを決める（marker() で '+'/'-'/' ' を取得）。
struct DiffLine
{
    LineOp op = LineOp::Context;
    std::string text; // 行の原文内容（行末改行は含まない）

    // 行内強調区間。Add は現在行、Delete はベースライン行に対する変更範囲。
    // Context 行・行全体が新規/消滅の場合は空（強調すべき部分差がない）。
    std::vector<InlineSpan> spans;

    // ベースライン側・現在側の 1 始まり行番号（unified 表示のガター用）。0 は該当なし。
    std::size_t old_line_no = 0; // ベースライン側行番号（Add 行は 0）
    std::size_t new_line_no = 0; // 現在側行番号（Delete 行は 0）

    // 色に依存しない判別記号。要件8.4「+/- 記号併用（色非依存）」。
    char marker() const
    {
        switch (op)
        {
        case LineOp::Add:
            return '+';
        case LineOp::Delete:
            return '-';
        case LineOp::Context:
        default:
            return ' ';
        }
    }
};

// 差分計算の結果。unified（インライン）形式の行列。
struct DiffResult
{
    std::vector<DiffLine> lines;

    // 入力が大きすぎる等でフル差分を計算せずフォールバックしたか（要件8章「10MB以上自動オフ」/
    // design.md 8章 I6「開始前に上限判定し超過時は計算を開始せずフォールバック」）。
    // true のとき lines は空または「大きすぎる」旨を示す最小情報のみ（UI が代替表示する）。
    bool truncated = false;

    // 協調キャンセルで中断したか（design.md 4章「古いタスクは協調キャンセル」）。
    // true のとき lines は途中までを持ち得るが、UI は破棄して最新リクエストを待つ。
    bool cancelled = false;

    // 集計（バッジ・通知の出し分け用）。truncated 時は 0。
    std::size_t added = 0;   // 追加行数
    std::size_t removed = 0; // 削除行数
};

// 差分計算の上限（要件8章「差分は…10MB以上自動オフ」/ design.md 8章 I6）。
// dtl（Myers）は途中中断できないため、入力サイズ（行数・最長行長）で開始前に判定する
// （タイムアウトを別スレッド中断に頼らない）。閾値は設定で変更可能。
struct DiffLimits
{
    // 片側の総バイト数の上限（既定 10MB）。いずれかの入力がこれを超えたら計算を開始しない。
    std::size_t max_total_bytes = 10u * 1024u * 1024u;
    // 片側の総行数の上限（行数爆発＝Myers の O(ND) 悪化を開始前に断つ）。
    std::size_t max_lines = 200'000u;
    // 1 行の最長バイト数の上限（超長行＝行内 LCS の O(n*m) 悪化を開始前に断つ）。
    std::size_t max_line_bytes = 100'000u;
    // 相違量（編集距離 D の上限近似）。同値行オーバーラップから求めた両側の非共通行数がともに
    // これを超えたら計算を開始しない。dtl の O(N·D) は行数ガードを通っても D 由来で暴走するため、
    // 「行数は N を縛るだけで D を縛らない」隙を塞ぐ（design.md
    // 8章。全追加/全削除・散在編集は通す）。
    std::size_t max_diff_lines = 50'000u;
};

} // namespace pika::core::diff
