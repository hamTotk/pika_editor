#include "core/render/preview_features.h"

#include <cctype>
#include <string>

namespace pika::core::render
{

namespace
{

// 行を [begin, end) で走査するための軽量ヘルパ。改行は LF/CRLF 両対応。
struct LineView
{
    std::string_view text; // 改行を含まない 1 行
    std::size_t next;      // 次行の開始オフセット
};

LineView next_line(std::string_view s, std::size_t pos)
{
    const std::size_t nl = s.find('\n', pos);
    if (nl == std::string_view::npos)
    {
        return {s.substr(pos), s.size()};
    }
    std::size_t end = nl;
    if (end > pos && s[end - 1] == '\r')
    {
        --end; // CRLF の \r を行末から除く
    }
    return {s.substr(pos, end - pos), nl + 1};
}

// 先頭の空白（スペース/タブ）を除いた部分を返す。
std::string_view ltrim(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
    {
        ++i;
    }
    return s.substr(i);
}

// 末尾の空白を除く。
std::string_view rtrim(std::string_view s)
{
    std::size_t n = s.size();
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
    {
        --n;
    }
    return s.substr(0, n);
}

// 行が fenced code block のフェンス（``` または ~~~ が 3 つ以上）か。フェンス文字と
// 個数を返す（閉じフェンス照合に使う）。fenced でなければ count=0。
struct Fence
{
    char ch = 0;           // '`' または '~'
    std::size_t count = 0; // フェンス文字の連続数（3 以上で有効）
    std::string_view info; // 情報文字列（開きフェンスのみ意味を持つ。trim 済み）
};

Fence parse_fence(std::string_view line)
{
    std::string_view t = ltrim(line);
    if (t.empty() || (t[0] != '`' && t[0] != '~'))
    {
        return {};
    }
    const char ch = t[0];
    std::size_t count = 0;
    while (count < t.size() && t[count] == ch)
    {
        ++count;
    }
    if (count < 3)
    {
        return {};
    }
    Fence f;
    f.ch = ch;
    f.count = count;
    f.info = rtrim(ltrim(t.substr(count)));
    return f;
}

// 情報文字列の先頭トークン（言語名）を小文字で取り出す。空白/タブで区切る。
std::string first_token_lower(std::string_view info)
{
    std::size_t i = 0;
    while (i < info.size() && info[i] != ' ' && info[i] != '\t')
    {
        ++i;
    }
    std::string tok(info.substr(0, i));
    for (char& c : tok)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return tok;
}

// 数字（ASCII 0-9）か。通貨判定に使う。
bool is_ascii_digit(char c)
{
    return c >= '0' && c <= '9';
}

// 本文行（コードフェンス外）に数式デリミタがあるか。
//   - `$$`（ブロック数式の開始）を含む。
//   - `$...$`（同一行内で開いて閉じるインライン数式）。
//
// 通貨表記との誤検出回避（markdown-it / pandoc 系の定番ルール）：
//   * 開き `$` の直後が数字なら、その `$` はインライン数式の開始としない
//     （`$5`・`$100` は通貨）。これで「価格は $5 と $9 です」のような同一行
//     2 個の通貨 `$` を数式化しない。
//   * 開き `$` の直後が空白も数式でない（KaTeX auto-render の既定挙動に一致）。
//   * 閉じ `$` の直前が空白なら数式でない。閉じ `$` の直後が数字なら
//     数式でない（次の通貨の開始とみなす。$x ... $5）。
// 本物の数式（`$x=1$`＝直後が 'x'、`$$E=mc^2$$`＝ブロック）はこのルールに
// 掛からず従来どおり検出する。このルールは描画側（assets/preview-bootstrap.js
// の renderMath 前処理）と必ず一致させる（整合が肝）。
bool line_has_math(std::string_view line)
{
    // ブロック数式 `$$`。
    if (line.find("$$") != std::string_view::npos)
    {
        return true;
    }
    // インライン `$...$`：開き候補 → 同一行内の閉じ候補を探す。
    for (std::size_t i = 0; i + 1 < line.size(); ++i)
    {
        if (line[i] != '$')
        {
            continue;
        }
        // バックスラッシュでエスケープされた \$ は数式デリミタでない。
        if (i > 0 && line[i - 1] == '\\')
        {
            continue;
        }
        const char after = line[i + 1];
        if (after == ' ' || after == '\t' || after == '$')
        {
            continue; // 開き直後が空白 or もう一つの $（=$$ は上で処理済み）。
        }
        if (is_ascii_digit(after))
        {
            continue; // 開き直後が数字＝通貨（$5・$100）。数式の開始としない。
        }
        // 同一行内で閉じ `$` を探す（直前が非空白・非エスケープ、直後が非数字）。
        for (std::size_t j = i + 1; j < line.size(); ++j)
        {
            if (line[j] != '$' || line[j - 1] == '\\')
            {
                continue;
            }
            const char before = line[j - 1];
            if (before == ' ' || before == '\t')
            {
                continue; // 閉じ直前が空白は数式でない。
            }
            if (j + 1 < line.size() && is_ascii_digit(line[j + 1]))
            {
                continue; // 閉じ直後が数字＝次の通貨の開始（$x ... $5）。数式の閉じとしない。
            }
            return true; // $...$ が同一行で閉じた。
        }
    }
    return false;
}

} // namespace

PreviewFeatures detect_preview_features(std::string_view markdown)
{
    PreviewFeatures feat;

    bool in_fence = false;       // コードフェンス内か
    char fence_ch = 0;           // 開いているフェンス文字
    std::size_t fence_count = 0; // 開きフェンスの個数（閉じは同種・同数以上）

    std::size_t pos = 0;
    while (pos < markdown.size())
    {
        const LineView lv = next_line(markdown, pos);
        pos = lv.next;
        const std::string_view line = lv.text;

        if (in_fence)
        {
            // 閉じフェンス（同種・同数以上・情報文字列なし）で抜ける。
            const Fence f = parse_fence(line);
            if (f.count >= fence_count && f.ch == fence_ch && f.info.empty())
            {
                in_fence = false;
                fence_ch = 0;
                fence_count = 0;
            }
            // フェンス内のテキストは math 検出の対象にしない（コード中の $ を誤検出しない）。
            continue;
        }

        const Fence f = parse_fence(line);
        if (f.count >= 3)
        {
            // 開きフェンス：情報文字列で mermaid / 言語付きコードを判定する。
            const std::string lang = first_token_lower(f.info);
            if (lang == "mermaid")
            {
                feat.mermaid = true;
            }
            else if (!lang.empty())
            {
                feat.code = true; // 言語付きコードブロック（highlight.js 対象）。
            }
            in_fence = true;
            fence_ch = f.ch;
            fence_count = f.count;
            continue;
        }

        // 本文行：数式デリミタを判定する。
        if (!feat.math && line_has_math(line))
        {
            feat.math = true;
        }
    }

    return feat;
}

} // namespace pika::core::render
