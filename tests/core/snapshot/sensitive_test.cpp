// core/snapshot 機密判定の検証（sprint5 must「機密ファイル(.env/*.key/*.pem/*secret*)は内容を
// 保存せず baselineHash のみ記録」の判定層）。既定パターンの一致・非一致・大小無視・区切りを観測。
#include "core/snapshot/sensitive.h"

#include <gtest/gtest.h>

namespace
{

using pika::core::snapshot::is_sensitive_default;

TEST(SensitiveTest, MatchesDotEnv)
{
    EXPECT_TRUE(is_sensitive_default(".env"));
    EXPECT_TRUE(is_sensitive_default("config/.env"));
    EXPECT_TRUE(is_sensitive_default(".env.local")); // *.env も既定パターンに含む
}

TEST(SensitiveTest, MatchesKeyAndPem)
{
    EXPECT_TRUE(is_sensitive_default("server.key"));
    EXPECT_TRUE(is_sensitive_default("certs/private.pem"));
    EXPECT_TRUE(is_sensitive_default("a/b/c/id_rsa.key"));
}

TEST(SensitiveTest, MatchesSecretSubstring)
{
    EXPECT_TRUE(is_sensitive_default("my_secret_token.txt"));
    EXPECT_TRUE(is_sensitive_default("SECRETS.md")); // 大小無視
    EXPECT_TRUE(is_sensitive_default("dir/app-secret-config"));
}

TEST(SensitiveTest, IgnoresNonSensitive)
{
    EXPECT_FALSE(is_sensitive_default("README.md"));
    EXPECT_FALSE(is_sensitive_default("src/main.cpp"));
    EXPECT_FALSE(is_sensitive_default("notes.txt"));
    // パスの途中ディレクトリ名に key が含まれてもファイル名で判定する。
    EXPECT_FALSE(is_sensitive_default("keystore/readme.md"));
}

TEST(SensitiveTest, BackslashPathSeparator)
{
    // Windows パス区切りでもファイル名抽出が効く。
    EXPECT_TRUE(is_sensitive_default("config\\.env"));
    EXPECT_TRUE(is_sensitive_default("certs\\private.pem"));
}

} // namespace
