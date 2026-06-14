// util/logger の検証（sprint 2・should）。
// Logger がファイル内容を書かず、パス・操作・エラーのみを記録する方針であることを観測する
// （要件12.4・design.md 12章「診断ログ（内容を書かない）」）。
#include "util/logger.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{

using pika::util::Logger;
using pika::util::LogLevel;

TEST(LoggerTest, FormatsOpPathDetail)
{
    const std::string line = Logger::format_line(LogLevel::Error, "save", "C:/ws/a.md", "code=Io");
    EXPECT_EQ(line, "ERROR op=save path=C:/ws/a.md detail=code=Io");
}

TEST(LoggerTest, OmitsDetailWhenEmpty)
{
    const std::string line = Logger::format_line(LogLevel::Info, "open", "C:/ws/a.md", "");
    EXPECT_EQ(line, "INFO op=open path=C:/ws/a.md");
}

TEST(LoggerTest, SinkReceivesFormattedLine)
{
    std::vector<std::string> captured;
    Logger log([&captured](LogLevel, const std::string& l) { captured.push_back(l); });
    log.warn("watch", "C:/ws", "overflow");
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0], "WARN op=watch path=C:/ws detail=overflow");
}

TEST(LoggerTest, ApiHasNoChannelForFileContent)
{
    // ログ API はパス・操作・非機密メタのみを受け取り、内容を渡す経路を持たない。
    // 仮にファイル内容を detail に「渡そうとしても」整形行はそのメタ枠に閉じ、
    // 内容そのものをロギングする専用 API は存在しない（構造的な内容混入防止）。
    std::vector<std::string> captured;
    Logger log([&captured](LogLevel, const std::string& l) { captured.push_back(l); });
    // 呼び出し側がメタとして渡した文字列のみが出る。content 引数は存在しない。
    log.info("read", "C:/ws/secret.env", "bytes=1024");
    ASSERT_EQ(captured.size(), 1u);
    // 行に現れるのは op/path/detail のメタのみ。
    EXPECT_EQ(captured[0], "INFO op=read path=C:/ws/secret.env detail=bytes=1024");
}

TEST(LoggerTest, NoSinkIsSafe)
{
    Logger log; // sink なし（ログ無効）でもクラッシュしない
    log.error("save", "C:/ws/a.md", "code=Io");
    SUCCEED();
}

} // namespace
