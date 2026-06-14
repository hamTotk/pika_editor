#include "core/ipc/cli_parser.h"

#include <cctype>

namespace pika::core::ipc
{

namespace
{

// 末尾の `:<整数>` を 1 段剥がす。剥がせたら value に整数を入れて true、残りは head に縮める。
// ドライブレター直後のコロン（`C:` の 2 文字目）は分割対象にしないため、コロン位置が 1 のときは
// 剥がさない（要件3.1「先頭のドライブレター直後のコロンは分割しない」）。
bool strip_trailing_int(std::string& head, int& value)
{
    const std::size_t colon = head.rfind(':');
    if (colon == std::string::npos)
    {
        return false;
    }
    // ドライブレター直後（`C:` の位置 1）は分割しない。
    // 位置 0（先頭がコロン）も不正なため剥がさない。
    if (colon <= 1)
    {
        return false;
    }
    const std::string digits = head.substr(colon + 1);
    if (digits.empty())
    {
        return false;
    }
    // 整数でない（数字以外を含む）なら位置指定として剥がさない。
    for (char c : digits)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
    }
    // オーバーフロー回避のため上限を設けて積み上げる（実用上の行/桁は十分収まる）。
    long long n = 0;
    for (char c : digits)
    {
        n = n * 10 + (c - '0');
        if (n > 2'000'000'000LL)
        {
            n = 2'000'000'000LL;
        }
    }
    value = static_cast<int>(n);
    head = head.substr(0, colon);
    return true;
}

} // namespace

GotoParse parse_goto(const std::string& spec)
{
    GotoParse out;
    std::string head = spec;

    // 末尾から最大 2 段（桁→行）剥がす。1 段目が桁、2 段目が行。
    // 1 段しか剥がせなければそれが行（桁省略＝行頭）。0 段なら位置指定なし。
    int first = 0;
    if (!strip_trailing_int(head, first))
    {
        out.path = spec;
        return out;
    }

    int second = 0;
    if (strip_trailing_int(head, second))
    {
        // 2 段剥がせた: second=行, first=桁。
        out.path = head;
        out.target.has_position = true;
        out.target.line = second;
        out.target.column = first;
    }
    else
    {
        // 1 段のみ: first=行, 桁省略＝行頭。
        out.path = head;
        out.target.has_position = true;
        out.target.line = first;
        out.target.column = 1;
    }
    return out;
}

ParseResult parse_argv(const std::vector<std::string>& argv)
{
    ParseResult out;
    CliInvocation& inv = out.invocation;

    for (std::size_t i = 0; i < argv.size(); ++i)
    {
        const std::string& a = argv[i];
        if (a == "--help" || a == "-h")
        {
            inv.show_help = true;
            return out;
        }
        if (a == "--version")
        {
            inv.show_version = true;
            return out;
        }
        if (a == "-g")
        {
            inv.goto_mode = true;
            // -g の直後 1 引数を spec として扱う。spec が欠けていれば不正引数。
            if (i + 1 >= argv.size())
            {
                out.errored = true;
                out.message = "-g に位置指定が指定されていません";
                return out;
            }
            inv.paths.push_back(argv[i + 1]);
            ++i;
            continue;
        }
        // それ以外のオプション（先頭 '-' で2文字以上）は未知オプション＝不正引数。
        // 単独の '-'（size 1）はこの条件に入らず通常パスとして受理する（stdin 慣習等）。
        if (a.size() >= 2 && a[0] == '-')
        {
            // ドライブレターやパスではない未知オプション。
            out.errored = true;
            out.message = "認識できないオプションです";
            return out;
        }
        inv.paths.push_back(a);
    }
    return out;
}

namespace
{

// パス末尾がディレクトリ区切り（'/' または '\\'）なら「フォルダ意図」とみなす。
bool ends_with_separator(const std::string& p)
{
    return !p.empty() && (p.back() == '/' || p.back() == '\\');
}

} // namespace

ValidationResult validate(const CliInvocation& inv, const PathProbe& probe)
{
    ValidationResult out;

    // help/version は GUI を起動しない受理（コンソールスタブ側で出力する）。引数分類は不要。
    if (inv.show_help || inv.show_version)
    {
        out.accepted = true;
        out.exit_code = ExitCode::Accepted;
        return out;
    }

    int folder_count = 0;
    for (const std::string& raw : inv.paths)
    {
        ClassifiedArg arg;

        // -g モードのときだけ位置指定を剥がす。通常モードのパスはコロンを位置指定として
        // 扱わない（要件3.1「-g のときに位置指定をパースする」）。
        std::string path = raw;
        if (inv.goto_mode)
        {
            GotoParse g = parse_goto(raw);
            path = g.path;
            arg.target = g.target;
        }
        arg.path = path;

        const bool is_dir = probe.is_dir && probe.is_dir(path);
        const bool is_file = probe.is_file && probe.is_file(path);

        if (is_dir)
        {
            arg.kind = ArgKind::ExistingFolder;
            ++folder_count;
            if (folder_count > 1)
            {
                // 単一フォルダ方針（要件3.1）。複数フォルダ指定は不正引数。
                out.accepted = false;
                out.exit_code = ExitCode::InvalidArgument;
                out.message = "フォルダは 1 つまでしか指定できません";
                return out;
            }
        }
        else if (is_file)
        {
            arg.kind = ArgKind::ExistingFile;
        }
        else if (ends_with_separator(path))
        {
            // 末尾区切り＝フォルダ意図かつ実在しない → エラー
            // （要件3.2「存在しないフォルダはエラー」）。
            out.accepted = false;
            out.exit_code = ExitCode::InvalidArgument;
            out.message = "指定されたフォルダが存在しません";
            return out;
        }
        else
        {
            // 実在しないファイルパス → 新規タブ扱いとして受理（要件3.2）。
            arg.kind = ArgKind::NewFile;
        }

        out.args.push_back(std::move(arg));
    }

    // ここまで来たら受理（引数なし＝前回状態復元も受理。要件3.1）。
    out.accepted = true;
    out.exit_code = ExitCode::Accepted;
    return out;
}

ConsoleOutcome decide_console_outcome(const ParseResult& parsed, const ValidationResult* validation)
{
    ConsoleOutcome out;

    // 1) parse 失敗：具体的な理由（parsed.message）を見せて非0で終了する（一律文言に潰さない）。
    if (parsed.errored)
    {
        out.exit_code = static_cast<int>(ExitCode::InvalidArgument);
        out.error_text = parsed.message.empty() ? "不正な引数です" : parsed.message;
        return out;
    }

    // 2) help / version：GUI を起動せずコンソール出力して 0 で終了する（要件3）。
    if (parsed.invocation.show_help)
    {
        out.print_help = true;
        return out;
    }
    if (parsed.invocation.show_version)
    {
        out.print_version = true;
        return out;
    }

    // 3) 引数検証の失敗：validate の終了コードとメッセージで非0終了する。
    if (validation != nullptr && !validation->accepted)
    {
        out.exit_code = static_cast<int>(validation->exit_code);
        out.error_text = validation->message.empty() ? "引数が不正です" : validation->message;
        return out;
    }

    // 4) 受理（転送/前回状態復元へ進む）。実際の GUI 起動は main_gui が担う。
    out.exit_code = static_cast<int>(ExitCode::Accepted);
    return out;
}

} // namespace pika::core::ipc
