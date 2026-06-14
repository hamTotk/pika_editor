// core/render CSP 組み立ての検証（sprint4 must / should）。
// script-src を https://app.pika のみに限定し、リモート許可オフ時に外部 http(s) を含まない
// 文字列を生成すること、許可オン時のみ img-src/font-src/style-src に http: https: を追加することを
// 観測する（design.md 6章 CSP テンプレート / 要件2.4・6.2）。
#include "core/render/csp_builder.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::render::build_csp;
using pika::core::render::RemoteResourcePolicy;

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

TEST(CspBuilderTest, ScriptSrcIsAppPikaOnly)
{
    std::string csp = build_csp(RemoteResourcePolicy::Blocked);
    EXPECT_TRUE(contains(csp, "script-src https://app.pika;"));
    // script-src に外部や doc.pika が混ざらない（ユーザー文書 JS を実行しない境界）。
    EXPECT_FALSE(contains(csp, "script-src https://app.pika https"));
    EXPECT_FALSE(contains(csp, "script-src https://app.pika https://doc.pika"));
}

TEST(CspBuilderTest, DefaultSrcIsNone)
{
    std::string csp = build_csp(RemoteResourcePolicy::Blocked);
    EXPECT_TRUE(contains(csp, "default-src 'none';"));
}

TEST(CspBuilderTest, BlockedPolicyHasNoExternalHttp)
{
    std::string csp = build_csp(RemoteResourcePolicy::Blocked);
    // 既定（遮断）では外部 http: https: を一切含まない。
    EXPECT_FALSE(contains(csp, "http://"));
    // img-src は app.pika / doc.pika / data: のみ。
    EXPECT_TRUE(contains(csp, "img-src https://app.pika https://doc.pika data:"));
    // style-src は unsafe-inline 込みだが外部は無い。
    EXPECT_TRUE(contains(csp, "style-src https://app.pika 'unsafe-inline'"));
    EXPECT_FALSE(contains(csp, "style-src https://app.pika 'unsafe-inline' https:"));
}

TEST(CspBuilderTest, AllowedPolicyAddsExternalToImgFontStyle)
{
    std::string csp = build_csp(RemoteResourcePolicy::Allowed);
    // 許可オン時のみ img-src/font-src/style-src に http: https: を足す。
    EXPECT_TRUE(contains(csp, "style-src https://app.pika 'unsafe-inline' https: http:"));
    EXPECT_TRUE(contains(csp, "font-src https://app.pika https: http:"));
    EXPECT_TRUE(contains(csp, "img-src https://app.pika https://doc.pika data: https: http:"));
    // 許可オンでも script-src は app.pika のみ（外部 JS を足さない）。
    EXPECT_TRUE(contains(csp, "script-src https://app.pika;"));
}

TEST(CspBuilderTest, NeverAllowsObjectOrFrameSrc)
{
    // default-src 'none' により object-src/frame-src は常時遮断（明示追加しない）。
    std::string csp = build_csp(RemoteResourcePolicy::Allowed);
    EXPECT_FALSE(contains(csp, "object-src"));
    EXPECT_FALSE(contains(csp, "frame-src"));
}

} // namespace
