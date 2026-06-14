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
    // 出力（stdout/stderr）を UTF-8 にする。これがないと日本語の help/version/診断が既定の
    // コンソールコードページ（例: CP932）で化け、パイプ・リダイレクト先でも壊れる（要件3 should
    // 「--help/--version がパイプ・リダイレクトで欠落・文字化けせず取得できる」）。ソースは /utf-8
    // でコンパイルされ文字列リテラルは UTF-8 バイト列なので、出力 CP を合わせれば一貫する。
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }

    auto parsed = pika::core::ipc::parse_argv(args);
    if (parsed.errored)
    {
        // 具体的な失敗理由（コアが返す message。例:「認識できないオプションです」「-g に位置指定が
        // 指定されていません」）をそのまま見せる。一律「不正な引数です」で握り潰さない（sprint7
        // high）。
        std::fputs("pika: ", stderr);
        std::fputs(parsed.message.empty() ? "不正な引数です" : parsed.message.c_str(), stderr);
        std::fputc('\n', stderr);
        return static_cast<int>(pika::core::ipc::ExitCode::InvalidArgument);
    }

    if (parsed.invocation.show_help)
    {
        print_help();
        return 0;
    }
    if (parsed.invocation.show_version)
    {
        // パイプ・リダイレクトでも欠落しないよう puts/printf（行バッファ）で出力する。
        std::printf("pika %s\n", kVersion);
        return 0;
    }

    // ウィンドウ生成・転送の前に引数検証を同期実行する（要件3.2）。
    pika::core::ipc::PathProbe probe;
    probe.is_dir = path_is_dir;
    probe.is_file = path_is_file;
    auto v = pika::core::ipc::validate(parsed.invocation, probe);
    if (!v.accepted)
    {
        std::fputs("pika: ", stderr);
        std::fputs(v.message.empty() ? "引数が不正です" : v.message.c_str(), stderr);
        std::fputc('\n', stderr);
        return static_cast<int>(v.exit_code);
    }

    // 受理。実際の GUI 起動/転送は GUI 本体（main_gui）が担うため、ここでは受理＝0 を返す。
    return static_cast<int>(pika::core::ipc::ExitCode::Accepted);
}
