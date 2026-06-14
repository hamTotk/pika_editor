// core/render レンダリング暴走ガード判定の検証（sprint4 must）。
// 画像6000万px・SVG展開8000万px相当/要素5万・HTML要素数/ネスト深さの閾値超過を入力サイズから
// 開始前に判定し、超過ならレンダリング不可フラグを返すことを観測する（要件2.2）。
#include "core/render/render_guard.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::render::guard_html;
using pika::core::render::guard_image;
using pika::core::render::guard_svg;
using pika::core::render::RenderGuardLimits;

TEST(RenderGuardTest, ImageWithinLimitIsAllowed)
{
    RenderGuardLimits lim; // 既定6000万px
    // 6000x6000 = 3600万px < 6000万px。
    auto v = guard_image(6000, 6000, lim);
    EXPECT_TRUE(v.allowed);
}

TEST(RenderGuardTest, ImageAtExactLimitIsAllowed)
{
    RenderGuardLimits lim;
    // ちょうど上限（6000万px）は許可（境界＝上限以下を許可）。
    auto v = guard_image(60'000'000ull, 1, lim);
    EXPECT_TRUE(v.allowed);
}

TEST(RenderGuardTest, ImageOverLimitIsBlocked)
{
    RenderGuardLimits lim;
    // 8000x8000 = 6400万px > 6000万px。
    auto v = guard_image(8000, 8000, lim);
    EXPECT_FALSE(v.allowed);
    EXPECT_FALSE(v.reason.empty());
}

TEST(RenderGuardTest, ZeroDimensionImageIsBlocked)
{
    RenderGuardLimits lim;
    EXPECT_FALSE(guard_image(0, 100, lim).allowed);
    EXPECT_FALSE(guard_image(100, 0, lim).allowed);
}

TEST(RenderGuardTest, MultiplicationOverflowImageIsBlocked)
{
    RenderGuardLimits lim;
    // width*height がオーバーフローする巨大値は不可（安全側）。
    auto v = guard_image(0xFFFFFFFFFFFFFFFFull, 2, lim);
    EXPECT_FALSE(v.allowed);
}

TEST(RenderGuardTest, SvgWithinPixelsAndElementsIsAllowed)
{
    RenderGuardLimits lim;                     // 8000万px / 5万要素
    auto v = guard_svg(8000, 8000, 1000, lim); // 6400万px, 1000要素
    EXPECT_TRUE(v.allowed);
}

TEST(RenderGuardTest, SvgOverPixelsIsBlocked)
{
    RenderGuardLimits lim;
    // 9000x9000 = 8100万px > 8000万px。
    auto v = guard_svg(9000, 9000, 10, lim);
    EXPECT_FALSE(v.allowed);
}

TEST(RenderGuardTest, SvgOverElementsIsBlocked)
{
    RenderGuardLimits lim;
    // ピクセルは小さいが要素数が5万超。
    auto v = guard_svg(100, 100, 50'001, lim);
    EXPECT_FALSE(v.allowed);
}

TEST(RenderGuardTest, SvgAtElementLimitIsAllowed)
{
    RenderGuardLimits lim;
    auto v = guard_svg(100, 100, 50'000, lim);
    EXPECT_TRUE(v.allowed);
}

TEST(RenderGuardTest, HtmlWithinLimitsIsAllowed)
{
    RenderGuardLimits lim;
    auto v = guard_html(1000, 50, lim);
    EXPECT_TRUE(v.allowed);
}

TEST(RenderGuardTest, HtmlOverElementCountIsBlocked)
{
    RenderGuardLimits lim;
    auto v = guard_html(500'001, 10, lim);
    EXPECT_FALSE(v.allowed);
}

TEST(RenderGuardTest, HtmlOverNestDepthIsBlocked)
{
    RenderGuardLimits lim;
    auto v = guard_html(10, 513, lim);
    EXPECT_FALSE(v.allowed);
}

TEST(RenderGuardTest, ConfigurableLimitsCanRelax)
{
    // 上限はすべて設定で緩和できる（要件2.2）。緩めれば許可される。
    RenderGuardLimits lim;
    lim.max_image_pixels = 200'000'000ull;
    auto v = guard_image(10000, 10000, lim); // 1億px
    EXPECT_TRUE(v.allowed);
}

} // namespace
