// core/render/preview_features の検証（F-004）。
// 同梱スクリプト（Mermaid/KaTeX/highlight.js）の注入を「該当記法がある時だけ」に絞る判定の正しさ
// （未使用時コストゼロ＝原則③ の根拠）。コードフェンス内の $ を数式と誤検出しないことも確認する。
#include "core/render/preview_features.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::render::detect_preview_features;
using pika::core::render::PreviewFeatures;

// ---- 何も該当しない（素の Markdown）＝一切注入しない ---- //

TEST(PreviewFeaturesTest, PlainMarkdownDetectsNothing)
{
    const PreviewFeatures f =
        detect_preview_features("# 見出し\n\n本文と **強調** と [link](a.md)。\n");
    EXPECT_FALSE(f.mermaid);
    EXPECT_FALSE(f.math);
    EXPECT_FALSE(f.code);
    EXPECT_FALSE(f.any());
}

TEST(PreviewFeaturesTest, EmptyDetectsNothing)
{
    const PreviewFeatures f = detect_preview_features("");
    EXPECT_FALSE(f.any());
}

// ---- Mermaid ---- //

TEST(PreviewFeaturesTest, MermaidFenceDetected)
{
    const PreviewFeatures f = detect_preview_features("```mermaid\ngraph TD; A-->B;\n```\n");
    EXPECT_TRUE(f.mermaid);
    EXPECT_FALSE(f.code); // mermaid は code（highlight.js 対象）には数えない。
}

TEST(PreviewFeaturesTest, MermaidInfoStringCaseAndSpaceTolerant)
{
    const PreviewFeatures f = detect_preview_features("```  Mermaid  \nsequenceDiagram\n```\n");
    EXPECT_TRUE(f.mermaid);
}

// ---- コードブロック（言語付き）→ highlight.js ---- //

TEST(PreviewFeaturesTest, FencedCodeWithLanguageDetectsCode)
{
    const PreviewFeatures f = detect_preview_features("```cpp\nint main(){}\n```\n");
    EXPECT_TRUE(f.code);
    EXPECT_FALSE(f.mermaid);
    EXPECT_FALSE(f.math);
}

TEST(PreviewFeaturesTest, TildeFenceWithLanguageDetectsCode)
{
    const PreviewFeatures f = detect_preview_features("~~~python\nprint(1)\n~~~\n");
    EXPECT_TRUE(f.code);
}

TEST(PreviewFeaturesTest, FencedCodeWithoutLanguageDoesNotDetectCode)
{
    // 言語指定なしのコードフェンスはハイライト対象でない（注入しない）。
    const PreviewFeatures f = detect_preview_features("```\nplain text block\n```\n");
    EXPECT_FALSE(f.code);
    EXPECT_FALSE(f.mermaid);
}

// ---- 数式（KaTeX） ---- //

TEST(PreviewFeaturesTest, BlockMathDetected)
{
    const PreviewFeatures f = detect_preview_features("数式:\n\n$$ E = mc^2 $$\n");
    EXPECT_TRUE(f.math);
}

TEST(PreviewFeaturesTest, InlineMathDetected)
{
    const PreviewFeatures f = detect_preview_features("ここに $a^2 + b^2 = c^2$ がある。\n");
    EXPECT_TRUE(f.math);
}

TEST(PreviewFeaturesTest, SingleDollarNotClosedIsNotMath)
{
    // 通貨表記 1 個の $ だけ（同一行で閉じない）は数式と見なさない（誤検出回避）。
    const PreviewFeatures f = detect_preview_features("価格は $5 です。\n");
    EXPECT_FALSE(f.math);
}

// ---- 通貨 vs インライン数式のロバスト化（F-004 medium 修正） ---- //
// 採用ルール（markdown-it/pandoc 系）：開き `$` の直後が数字ならその `$` は数式開始としない
// （通貨）。検出側（line_has_math）と描画側（preview-bootstrap.js）で同一ルールを適用する。

TEST(PreviewFeaturesTest, TwoCurrencyDollarsSameLineIsNotMath)
{
    // 同一行に未エスケープ $ が 2 つある通貨表記をインライン数式と誤検出しない（本件の核心）。
    const PreviewFeatures f = detect_preview_features("価格は $5 と $9 です。\n");
    EXPECT_FALSE(f.math);
}

TEST(PreviewFeaturesTest, CurrencyWithMultipleDigitsIsNotMath)
{
    // $100 のような複数桁の通貨も数式と見なさない（$ 直後が数字）。
    const PreviewFeatures f = detect_preview_features("金額は $100 です。\n");
    EXPECT_FALSE(f.math);
}

TEST(PreviewFeaturesTest, RealInlineMathWithSpacesStillDetected)
{
    // 本物のインライン数式（$ 直後が 'x'）は引き続き検出する（回帰防止）。
    const PreviewFeatures f = detect_preview_features("解は $x = 1$ です。\n");
    EXPECT_TRUE(f.math);
}

TEST(PreviewFeaturesTest, BlockMathWithSpacesStillDetected)
{
    // ブロック数式 $$...$$ は通貨ルールに掛からず検出する（回帰防止）。
    const PreviewFeatures f = detect_preview_features("$$ E = mc^2 $$\n");
    EXPECT_TRUE(f.math);
}

TEST(PreviewFeaturesTest, MixedRealMathAndCurrencyDetectsMath)
{
    // 本物の数式 $x=1$ と通貨 $5 が混在する行：数式があるので math は立つ
    // （描画側は $5 を保護しつつ $x=1$ を描画する）。
    const PreviewFeatures f = detect_preview_features("解は $x=1$ で 価格は $5 です。\n");
    EXPECT_TRUE(f.math);
}

TEST(PreviewFeaturesTest, ClosingDollarFollowedByDigitIsNotMath)
{
    // 閉じ候補 $ の直後が数字なら次の通貨の開始とみなし、数式の閉じとしない
    // （$x ... $5 の $...$ を数式化しない）。
    const PreviewFeatures f = detect_preview_features("単価 $x と $5 の比較。\n");
    EXPECT_FALSE(f.math);
}

TEST(PreviewFeaturesTest, EscapedDollarIsNotMath)
{
    // バックスラッシュエスケープした \$ ... \$ は数式デリミタでない。
    const PreviewFeatures f = detect_preview_features("コストは \\$5 から \\$9 です。\n");
    EXPECT_FALSE(f.math);
}

// ---- コードフェンス内の $ を数式と誤検出しない（重要） ---- //

TEST(PreviewFeaturesTest, DollarInsideCodeFenceIsNotMath)
{
    const PreviewFeatures f = detect_preview_features("```bash\necho $HOME\nVAR=$x$y\n```\n");
    EXPECT_TRUE(f.code);  // 言語付きコードブロックなので code は立つ。
    EXPECT_FALSE(f.math); // フェンス内の $ は数式と見なさない。
    EXPECT_FALSE(f.mermaid);
}

TEST(PreviewFeaturesTest, MathOutsideButCodeFenceInside)
{
    const PreviewFeatures f = detect_preview_features(
        "インライン $x$ あり。\n\n```js\nlet p = $a + $b;\n```\n\n末尾。\n");
    EXPECT_TRUE(f.math); // フェンス外の $x$ は数式。
    EXPECT_TRUE(f.code); // js コードフェンス。
}

// ---- 複合 ---- //

TEST(PreviewFeaturesTest, AllThreeDetected)
{
    const std::string md = "# 全部入り\n\n"
                           "$$ \\int_0^1 x dx $$\n\n"
                           "```mermaid\ngraph LR; A-->B;\n```\n\n"
                           "```cpp\nint x = 0;\n```\n";
    const PreviewFeatures f = detect_preview_features(md);
    EXPECT_TRUE(f.math);
    EXPECT_TRUE(f.mermaid);
    EXPECT_TRUE(f.code);
    EXPECT_TRUE(f.any());
}

// ---- CRLF 改行でも同じ判定 ---- //

TEST(PreviewFeaturesTest, CrlfNewlinesHandled)
{
    const PreviewFeatures f = detect_preview_features("```cpp\r\nint main(){}\r\n```\r\n$$x$$\r\n");
    EXPECT_TRUE(f.code);
    EXPECT_TRUE(f.math);
}

// ---- 決定論（同一入力で同一出力） ---- //

TEST(PreviewFeaturesTest, Deterministic)
{
    const std::string md = "$$y$$\n```rust\nfn main(){}\n```\n";
    const PreviewFeatures a = detect_preview_features(md);
    const PreviewFeatures b = detect_preview_features(md);
    EXPECT_EQ(a.math, b.math);
    EXPECT_EQ(a.code, b.code);
    EXPECT_EQ(a.mermaid, b.mermaid);
}

} // namespace
