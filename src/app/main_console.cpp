// pika.com: コンソールサブシステムの薄いスタブ。
// 要件3章「--help/--version をコンソールに出力」「引数の検証はウィンドウ生成・転送の前に
// 同期実行し、検証失敗時はGUIを起動せず非0で即終了」「終了コード：0＝受理、非0＝エラー」。
//
// 引数の解釈・検証・分類・終了コードの判定本体は core/ipc（UI/Win32 非依存・gtest 検証済み）に
// 置き、ここは argv をコアへ渡し結果の終了コードを返す／help・version を出力する薄い橋渡しに
// 徹する。GUI 本体への橋渡し（受理時の転送/起動）はプラットフォーム層（main_gui 側）が担う。
#include "core/ipc/cli_parser.h"

#include <cstdio>
#include <string>
#include <vector>

#include <sys/stat.h>

// コンソール出力コードページを UTF-8 に設定するためだけに Win32 を使う（pika.com は Windows
// 専用のコンソールスタブ）。日本語の help/version/診断メッセージを UTF-8（pika の文字列方針）で
// 正しく表示・出力するため。NOMINMAX/lean ヘッダで取り込みを最小化する。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{

constexpr const char* kVersion = "0.0.1";

void print_help()
{
    std::puts("pika - Windows 向け超軽量 Markdown/HTML エディタ");
    std::puts("");
    std::puts("使い方:");
    std::puts(
        "  pika [<パス>...]        指定したファイル/フォルダを開く（引数なしで前回状態を復元）");
    std::puts("  pika -g <file>:<行>[:<桁>]  指定位置を開く（VS Code 互換）");
    std::puts("  pika --help             このヘルプを表示");
    std::puts("  pika --version          バージョンを表示");
}

// FS 実在判定（プラットフォーム層の責務）。コアの PathProbe に注入する。
bool path_is_dir(const std::string& p)
{
    struct _stat st;
    return _stat(p.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
}

bool path_is_file(const std::string& p)
{
    struct _stat st;
    return _stat(p.c_str(), &st) == 0 && (st.st_mode & _S_IFREG) != 0;
}

} // namespace

int main(int argc, char** argv)
{
    // パイプ・リダイレクト時の非文字化けは /utf-8 コンパイル（CMakeLists.txt）で文字列リテラルが
    // UTF-8 バイト列になることが担保する（CRT の puts/printf
    // はファイル/パイプへ生バイトを直接書き、
    // コンソールコードページを介さない）。SetConsoleOutputCP(CP_UTF8)
    // はこれとは別に、対話コンソール 表示時の化け（既定 CP932 等）を追加で防ぐ（要件3 should）。
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }

    const auto parsed = pika::core::ipc::parse_argv(args);

    // 引数検証は「parse 成功かつ help/version でない」ときだけ、ウィンドウ生成・転送の前に同期実行
    // する（要件3.2）。FS 実在判定はプラットフォーム層の述語をコアへ注入する。
    const bool need_validate =
        !parsed.errored && !parsed.invocation.show_help && !parsed.invocation.show_version;
    pika::core::ipc::ValidationResult v;
    if (need_validate)
    {
        pika::core::ipc::PathProbe probe;
        probe.is_dir = path_is_dir;
        probe.is_file = path_is_file;
        v = pika::core::ipc::validate(parsed.invocation, probe);
    }

    // 何を出力し・どの終了コードで終わるかの決定は、gtest 検証済みのコアロジックに委ねる
    // （終了コード/メッセージ選択の自己回帰。sprint7 high）。ここは実出力だけを行う薄い橋渡し。
    const pika::core::ipc::ConsoleOutcome outcome =
        pika::core::ipc::decide_console_outcome(parsed, need_validate ? &v : nullptr);

    if (outcome.print_help)
    {
        print_help();
    }
    if (outcome.print_version)
    {
        std::printf("pika %s\n", kVersion);
    }
    if (!outcome.error_text.empty())
    {
        std::fputs("pika: ", stderr);
        std::fputs(outcome.error_text.c_str(), stderr);
        std::fputc('\n', stderr);
    }
    // 受理時の実際の GUI 起動/転送は GUI 本体（main_gui）が担う。
    return outcome.exit_code;
}
