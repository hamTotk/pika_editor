// core/render: プレビュー本文がどの同梱スクリプトを要するかの検出（F-004。wx 非依存）。
// design.md 6章「Mermaid・KaTeX・ハイライトの JS/CSS はアプリに同梱し、該当記法が文書に存在する
// ときだけ <script> タグを出力する（遅延読み込み）」/ 設計原則③「使っていない機能のコストはゼロ」/
// 要件6.2/6.4（コードハイライト・Mermaid・数式 KaTeX）。
//
// md4c は `$`/`$$` を数式として解釈せず本文テキストとして素通しするため、数式の有無は
// Markdown ソースをスキャンして判定する（KaTeX auto-render が本文をスキャンしてレンダリングする）。
// Mermaid・コードブロックは fenced code block（``` / ~~~）の情報文字列で判定する。コードフェンス
// 内部の `$` は数式と誤検出しない（フェンスの開閉を追って本文 vs コードを区別する）。
//
// 純ロジック（WebView2 非依存）。gtest
// で各記法の検出/未検出を決定論検証する（注入の出し分け根拠）。
#pragma once

#include <string_view>

namespace pika::core::render
{

// プレビュー本文が要する同梱機能（該当時のみ <script>/<link> を注入する＝未使用時コストゼロ）。
struct PreviewFeatures
{
    bool mermaid = false; // ```mermaid フェンスがある（Mermaid 図）
    bool math = false;    // $$...$$ または $...$ の数式デリミタがある（KaTeX）
    bool code = false;    // 言語付き ```lang コードフェンスがある（highlight.js）

    bool any() const noexcept { return mermaid || math || code; }
};

// Markdown ソースをスキャンして必要な同梱機能を判定する（注入の出し分けに使う）。
//   - mermaid: 情報文字列が "mermaid"（大小無視・前後空白許容）の fenced code block。
//   - code:    情報文字列に言語名がある fenced code block（mermaid 自体は code には数えない）。
//   - math:    コードフェンス外の本文に `$$`（ブロック）または
//   `$...$`（インライン・同一行で閉じる）。
// コードフェンス内部のテキストは math 検出の対象にしない（コード中の `$` を数式と誤検出しない）。
PreviewFeatures detect_preview_features(std::string_view markdown);

} // namespace pika::core::render
