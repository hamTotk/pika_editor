// util/result の Result<T> の検証（sprint 2）。
// コア公開 API が例外を投げず成功値/エラー値を返すこと（design.md
// 15章「例外はモジュール内部に閉じる」）。
#include "util/result.h"

#include <string>

#include <gtest/gtest.h>

namespace
{

using pika::util::ErrorCode;
using pika::util::ErrorInfo;
using pika::util::Result;

// 例外を投げずエラー値で失敗を伝える擬似 API。
Result<int> parse_positive(int n)
{
    if (n <= 0)
    {
        return Result<int>::err(ErrorCode::InvalidArgument, "正の数ではありません");
    }
    return Result<int>::ok(n * 2);
}

TEST(ResultT, OkCarriesValue)
{
    auto r = parse_positive(21);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_FALSE(r.is_err());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultT, ErrCarriesCodeAndMessage)
{
    auto r = parse_positive(-1);
    ASSERT_TRUE(r.is_err());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "正の数ではありません");
}

TEST(ResultT, MoveOnlyValueWorks)
{
    auto make = []() -> Result<std::string> {
        return Result<std::string>::ok(std::string("moved"));
    };
    auto r = make();
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::move(r).value(), "moved");
}

TEST(ResultVoid, OkAndErr)
{
    auto ok = Result<void>::ok();
    EXPECT_TRUE(ok.is_ok());
    EXPECT_FALSE(ok.is_err());

    auto err = Result<void>::err(ErrorCode::Io, "書き込み失敗");
    EXPECT_TRUE(err.is_err());
    EXPECT_EQ(err.code(), ErrorCode::Io);
    EXPECT_EQ(err.error().message, "書き込み失敗");
}

TEST(ResultT, NoExceptionOnFailurePath)
{
    // 失敗経路で例外が飛ばないことを「投げたら EXPECT 失敗」で観測する。
    bool threw = false;
    try
    {
        auto r = parse_positive(0);
        EXPECT_TRUE(r.is_err());
    }
    catch (...)
    {
        threw = true;
    }
    EXPECT_FALSE(threw);
}

} // namespace
