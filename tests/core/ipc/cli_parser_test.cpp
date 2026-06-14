// core/ipc CLI 引数解釈の検証（sprint7 must「-g パース」「引数検証」「終了コード規約」）。
// -g の位置剥がし（ドライブレターのコロン非分割・桁省略・非整数無視）と、実在判定注入による
// フォルダ/ファイル/新規タブの分類・終了コードを観測する。
#include "core/ipc/cli_parser.h"

#include <set>

#include <gtest/gtest.h>

namespace
{

using pika::core::ipc::ArgKind;
using pika::core::ipc::CliInvocation;
using pika::core::ipc::ConsoleOutcome;
using pika::core::ipc::decide_console_outcome;
using pika::core::ipc::ExitCode;
using pika::core::ipc::parse_argv;
using pika::core::ipc::parse_goto;
using pika::core::ipc::ParseResult;
using pika::core::ipc::PathProbe;
using pika::core::ipc::validate;
using pika::core::ipc::ValidationResult;

// 実在集合を注入する PathProbe を作る（FS 非依存・決定論）。
PathProbe make_probe(std::set<std::string> dirs, std::set<std::string> files)
{
    PathProbe p;
    p.is_dir = [dirs](const std::string& path) { return dirs.count(path) != 0; };
    p.is_file = [files](const std::string& path) { return files.count(path) != 0; };
    return p;
}

// ---- parse_goto: -g の位置指定剥がし ----

TEST(CliGoto, LineAndColumn)
{
    auto g = parse_goto("doc.md:120:5");
    EXPECT_EQ(g.path, "doc.md");
    EXPECT_TRUE(g.target.has_position);
    EXPECT_EQ(g.target.line, 120);
    EXPECT_EQ(g.target.column, 5);
}

TEST(CliGoto, LineOnlyDefaultsColumnToLineStart)
{
    auto g = parse_goto("doc.md:120");
    EXPECT_EQ(g.path, "doc.md");
    EXPECT_TRUE(g.target.has_position);
    EXPECT_EQ(g.target.line, 120);
    EXPECT_EQ(g.target.column, 1); // 桁省略＝行頭
}

TEST(CliGoto, DriveLetterColonNotSplit)
{
    // `C:\dir\doc.md:42` のドライブレター直後のコロンは分割しない（要件3.1）。
    auto g = parse_goto("C:\\dir\\doc.md:42");
    EXPECT_EQ(g.path, "C:\\dir\\doc.md");
    EXPECT_TRUE(g.target.has_position);
    EXPECT_EQ(g.target.line, 42);
    EXPECT_EQ(g.target.column, 1);
}

TEST(CliGoto, DrivePathWithLineAndColumn)
{
    auto g = parse_goto("C:\\a\\b.md:10:3");
    EXPECT_EQ(g.path, "C:\\a\\b.md");
    EXPECT_EQ(g.target.line, 10);
    EXPECT_EQ(g.target.column, 3);
}

TEST(CliGoto, BareDrivePathHasNoPosition)
{
    // 末尾に :<整数> が無い（ドライブのコロンのみ）なら位置指定なし。
    auto g = parse_goto("C:\\dir\\doc.md");
    EXPECT_EQ(g.path, "C:\\dir\\doc.md");
    EXPECT_FALSE(g.target.has_position);
}

TEST(CliGoto, NonIntegerSuffixIgnored)
{
    // 行・桁が整数でない場合は位置指定を無視し全体をパスとして解釈（要件3.1）。
    auto g = parse_goto("doc.md:abc");
    EXPECT_EQ(g.path, "doc.md:abc");
    EXPECT_FALSE(g.target.has_position);
}

TEST(CliGoto, ColonInMiddleOnlyLastStripped)
{
    // ファイル名にコロンが無い前提でも、最後の :<整数> のみが対象。途中は触らない。
    auto g = parse_goto("a:b.md:7");
    EXPECT_EQ(g.path, "a:b.md");
    EXPECT_EQ(g.target.line, 7);
}

TEST(CliGoto, PlainPathNoColon)
{
    auto g = parse_goto("notes.txt");
    EXPECT_EQ(g.path, "notes.txt");
    EXPECT_FALSE(g.target.has_position);
}

// ---- parse_argv: オプション解釈 ----

TEST(CliParseArgv, HelpAndVersion)
{
    EXPECT_TRUE(parse_argv({"--help"}).invocation.show_help);
    EXPECT_TRUE(parse_argv({"-h"}).invocation.show_help);
    EXPECT_TRUE(parse_argv({"--version"}).invocation.show_version);
}

TEST(CliParseArgv, GotoTakesNextArgAsSpec)
{
    auto r = parse_argv({"-g", "doc.md:5"});
    EXPECT_FALSE(r.errored);
    EXPECT_TRUE(r.invocation.goto_mode);
    ASSERT_EQ(r.invocation.paths.size(), 1u);
    EXPECT_EQ(r.invocation.paths[0], "doc.md:5");
}

TEST(CliParseArgv, GotoWithoutSpecIsError)
{
    auto r = parse_argv({"-g"});
    EXPECT_TRUE(r.errored);
    // 失敗理由が具体的に設定される（スタブが一律「不正な引数です」で握り潰さないための材料。
    // sprint7 high）。
    EXPECT_FALSE(r.message.empty());
}

TEST(CliParseArgv, UnknownOptionIsError)
{
    auto r = parse_argv({"--frobnicate"});
    EXPECT_TRUE(r.errored);
    EXPECT_FALSE(r.message.empty());
}

TEST(CliParseArgv, ErrorMessageDistinguishesReason)
{
    // 異なる失敗理由には異なる具体メッセージが付く（理由の握り潰し防止＝fix の核心）。
    const auto unknown_opt = parse_argv({"--frobnicate"});
    const auto goto_no_spec = parse_argv({"-g"});
    ASSERT_TRUE(unknown_opt.errored);
    ASSERT_TRUE(goto_no_spec.errored);
    EXPECT_FALSE(unknown_opt.message.empty());
    EXPECT_FALSE(goto_no_spec.message.empty());
    EXPECT_NE(unknown_opt.message, goto_no_spec.message);
}

TEST(CliParseArgv, PathsCollected)
{
    auto r = parse_argv({"a.md", "b.md"});
    EXPECT_FALSE(r.errored);
    ASSERT_EQ(r.invocation.paths.size(), 2u);
    EXPECT_EQ(r.invocation.paths[0], "a.md");
}

// ---- validate: 分類と終了コード ----

TEST(CliValidate, ExistingFolderAccepted)
{
    CliInvocation inv;
    inv.paths = {"C:\\proj"};
    auto probe = make_probe({"C:\\proj"}, {});
    auto v = validate(inv, probe);
    EXPECT_TRUE(v.accepted);
    EXPECT_EQ(v.exit_code, ExitCode::Accepted);
    ASSERT_EQ(v.args.size(), 1u);
    EXPECT_EQ(v.args[0].kind, ArgKind::ExistingFolder);
}

TEST(CliValidate, ExistingFileAccepted)
{
    CliInvocation inv;
    inv.paths = {"C:\\proj\\a.md"};
    auto probe = make_probe({}, {"C:\\proj\\a.md"});
    auto v = validate(inv, probe);
    EXPECT_TRUE(v.accepted);
    EXPECT_EQ(v.args[0].kind, ArgKind::ExistingFile);
}

TEST(CliValidate, MissingFileAcceptedAsNewTab)
{
    // 存在しないファイルパスは新規タブ扱いとして受理（要件3.2）。
    CliInvocation inv;
    inv.paths = {"C:\\proj\\new.md"};
    auto probe = make_probe({}, {});
    auto v = validate(inv, probe);
    EXPECT_TRUE(v.accepted);
    EXPECT_EQ(v.exit_code, ExitCode::Accepted);
    EXPECT_EQ(v.args[0].kind, ArgKind::NewFile);
}

TEST(CliValidate, MissingFolderTrailingSeparatorIsError)
{
    // 末尾区切り＝フォルダ意図で実在しない → エラー（非0）。
    CliInvocation inv;
    inv.paths = {"C:\\proj\\nope\\"};
    auto probe = make_probe({}, {});
    auto v = validate(inv, probe);
    EXPECT_FALSE(v.accepted);
    EXPECT_EQ(v.exit_code, ExitCode::InvalidArgument);
    EXPECT_NE(static_cast<int>(v.exit_code), 0);
}

TEST(CliValidate, MultipleFoldersIsError)
{
    // 単一フォルダ方針（要件3.1）。複数フォルダ指定はエラー。
    CliInvocation inv;
    inv.paths = {"C:\\a", "C:\\b"};
    auto probe = make_probe({"C:\\a", "C:\\b"}, {});
    auto v = validate(inv, probe);
    EXPECT_FALSE(v.accepted);
    EXPECT_EQ(v.exit_code, ExitCode::InvalidArgument);
}

TEST(CliValidate, NoArgsAcceptedAsRestore)
{
    // 引数なし＝前回状態復元の受理（要件3.1）。
    CliInvocation inv;
    auto probe = make_probe({}, {});
    auto v = validate(inv, probe);
    EXPECT_TRUE(v.accepted);
    EXPECT_EQ(v.exit_code, ExitCode::Accepted);
}

TEST(CliValidate, GotoModeStripsPositionInClassifiedArg)
{
    // -g モードでは validate がパスから位置を剥がし、分類に target を載せる。
    CliInvocation inv;
    inv.goto_mode = true;
    inv.paths = {"C:\\proj\\a.md:99"};
    auto probe = make_probe({}, {"C:\\proj\\a.md"});
    auto v = validate(inv, probe);
    ASSERT_EQ(v.args.size(), 1u);
    EXPECT_EQ(v.args[0].path, "C:\\proj\\a.md");
    EXPECT_EQ(v.args[0].kind, ArgKind::ExistingFile);
    EXPECT_TRUE(v.args[0].target.has_position);
    EXPECT_EQ(v.args[0].target.line, 99);
}

TEST(CliValidate, FolderAndFileTogetherBothAccepted)
{
    // フォルダ＋ファイル同時指定（要件3.1）は両方受理（フォルダ 1 つまで）。
    CliInvocation inv;
    inv.paths = {"C:\\proj", "C:\\proj\\a.md"};
    auto probe = make_probe({"C:\\proj"}, {"C:\\proj\\a.md"});
    auto v = validate(inv, probe);
    EXPECT_TRUE(v.accepted);
    ASSERT_EQ(v.args.size(), 2u);
    EXPECT_EQ(v.args[0].kind, ArgKind::ExistingFolder);
    EXPECT_EQ(v.args[1].kind, ArgKind::ExistingFile);
}

// 終了コード規約: 受理=0 / 不正=非0 を明示的に観測する（要件3.2）。
TEST(CliValidate, ExitCodeContract)
{
    EXPECT_EQ(static_cast<int>(ExitCode::Accepted), 0);
    EXPECT_NE(static_cast<int>(ExitCode::InvalidArgument), 0);
}

// ---- decide_console_outcome: コンソールスタブのグルー（自己回帰網に乗せる）----

TEST(CliConsoleOutcome, UnknownOptionErrorsWithSpecificReason)
{
    const ConsoleOutcome o = decide_console_outcome(parse_argv({"--frobnicate"}), nullptr);
    EXPECT_NE(o.exit_code, 0);
    EXPECT_FALSE(o.error_text.empty()); // 具体理由（一律文言に潰さない）
    EXPECT_FALSE(o.print_help);
    EXPECT_FALSE(o.print_version);
}

TEST(CliConsoleOutcome, GotoWithoutSpecAndUnknownDifferInText)
{
    const ConsoleOutcome a = decide_console_outcome(parse_argv({"-g"}), nullptr);
    const ConsoleOutcome b = decide_console_outcome(parse_argv({"--frobnicate"}), nullptr);
    EXPECT_NE(a.exit_code, 0);
    EXPECT_NE(b.exit_code, 0);
    EXPECT_NE(a.error_text, b.error_text); // 失敗理由が区別される
}

TEST(CliConsoleOutcome, HelpReturnsZeroAndPrintsHelp)
{
    const ConsoleOutcome o = decide_console_outcome(parse_argv({"--help"}), nullptr);
    EXPECT_EQ(o.exit_code, 0);
    EXPECT_TRUE(o.print_help);
    EXPECT_TRUE(o.error_text.empty());
}

TEST(CliConsoleOutcome, VersionReturnsZeroAndPrintsVersion)
{
    const ConsoleOutcome o = decide_console_outcome(parse_argv({"--version"}), nullptr);
    EXPECT_EQ(o.exit_code, 0);
    EXPECT_TRUE(o.print_version);
}

TEST(CliConsoleOutcome, ValidationFailureReturnsNonZeroWithMessage)
{
    const auto parsed = parse_argv({"C:\\proj\\nope\\"}); // 末尾区切り＝存在しないフォルダ意図
    const ValidationResult v = validate(parsed.invocation, make_probe({}, {}));
    const ConsoleOutcome o = decide_console_outcome(parsed, &v);
    EXPECT_NE(o.exit_code, 0);
    EXPECT_FALSE(o.error_text.empty());
}

TEST(CliConsoleOutcome, AcceptedReturnsZeroNoText)
{
    const auto parsed = parse_argv({"a.md"});
    const ValidationResult v = validate(parsed.invocation, make_probe({}, {}));
    const ConsoleOutcome o = decide_console_outcome(parsed, &v);
    EXPECT_EQ(o.exit_code, 0);
    EXPECT_TRUE(o.error_text.empty());
    EXPECT_FALSE(o.print_help);
}

TEST(CliConsoleOutcome, EmptyParseMessageFallsBackToGenericText)
{
    ParseResult parsed;
    parsed.errored = true;
    parsed.message = ""; // 理由が空でも一般文言にフォールバックして無言にしない
    const ConsoleOutcome o = decide_console_outcome(parsed, nullptr);
    EXPECT_NE(o.exit_code, 0);
    EXPECT_FALSE(o.error_text.empty());
}

} // namespace
