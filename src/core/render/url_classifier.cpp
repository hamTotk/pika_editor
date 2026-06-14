#include "core/render/url_classifier.h"

#include <cctype>

namespace pika::core::render
{

namespace
{

char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

// HTML 数値文字参照（&#106; / &#x6A;）を 1 文字へ展開する。先頭が '&#' のときだけ処理し、
// 展開した文字（ASCII 範囲のみ）を out へ書き、消費長を返す。展開できなければ 0 を返す。
// スキーム偽装（java&#115;cript: 等）を無効化するための最小展開。
std::size_t decode_numeric_entity(std::string_view s, std::size_t pos, char& out)
{
    // s[pos] == '&' を前提に呼ぶ。
    std::size_t i = pos + 1;
    if (i >= s.size() || s[i] != '#')
    {
        return 0;
    }
    ++i;
    bool hex = false;
    if (i < s.size() && (s[i] == 'x' || s[i] == 'X'))
    {
        hex = true;
        ++i;
    }
    unsigned long code = 0;
    std::size_t digits = 0;
    while (i < s.size())
    {
        char c = s[i];
        int d = -1;
        if (c >= '0' && c <= '9')
        {
            d = c - '0';
        }
        else if (hex && c >= 'a' && c <= 'f')
        {
            d = c - 'a' + 10;
        }
        else if (hex && c >= 'A' && c <= 'F')
        {
            d = c - 'A' + 10;
        }
        if (d < 0)
        {
            break;
        }
        code = code * (hex ? 16u : 10u) + static_cast<unsigned long>(d);
        if (code > 0x10FFFF)
        {
            code = 0x10FFFF; // 飽和（巨大入力での暴走防止）
        }
        ++digits;
        ++i;
    }
    if (digits == 0)
    {
        return 0;
    }
    // 末尾の ';' は任意（ブラウザは ';' 無しでも解釈しうるため、あれば消費する）。
    if (i < s.size() && s[i] == ';')
    {
        ++i;
    }
    // ASCII のみ確定文字として返す（非 ASCII はスキーム判定に無関係。安全側で空白扱い）。
    out = (code < 0x80) ? static_cast<char>(code) : ' ';
    return i - pos;
}

} // namespace

std::string normalize_url_scheme_probe(std::string_view raw)
{
    std::string out;
    out.reserve(32);
    std::size_t i = 0;
    const std::size_t n = raw.size();

    // 先頭の空白・制御文字を読み飛ばす（ブラウザは URL 先頭の制御文字を無視するため）。
    while (i < n)
    {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c <= 0x20)
        {
            ++i;
            continue;
        }
        break;
    }

    // スキーム部（':' まで）を正規化する。':' 以降はスキーム判定に不要なので打ち切る。
    // ただし java\tscript: のような内部空白・制御文字も無視するため、コロン到達まで走査する。
    while (i < n && out.size() < 32)
    {
        char c = raw[i];
        if (c == '&')
        {
            char decoded = 0;
            std::size_t used = decode_numeric_entity(raw, i, decoded);
            if (used > 0)
            {
                if (decoded == ':')
                {
                    out.push_back(':');
                    break;
                }
                // 空白・制御は無視（スキーム内の難読化分割を潰す）。
                if (static_cast<unsigned char>(decoded) > 0x20)
                {
                    out.push_back(ascii_lower(decoded));
                }
                i += used;
                continue;
            }
            // 名前付き実体 &colon; / &Tab; などの代表だけ最小対応。
            if (raw.compare(i, 6, "&colon") == 0)
            {
                out.push_back(':');
                break;
            }
            // それ以外の '&' はそのまま 1 文字。
            out.push_back('&');
            ++i;
            continue;
        }
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20)
        {
            // スキーム内の空白・タブ・改行は無視（java\tscript: 対策）。
            ++i;
            continue;
        }
        if (c == ':')
        {
            out.push_back(':');
            break;
        }
        out.push_back(ascii_lower(c));
        ++i;
    }

    return out;
}

bool is_dangerous_url(std::string_view raw)
{
    std::string probe = normalize_url_scheme_probe(raw);
    // スキームが付いていない相対 URL（':' を含まない）は危険ではない。
    auto colon = probe.find(':');
    if (colon == std::string::npos)
    {
        return false;
    }
    std::string scheme = probe.substr(0, colon);
    if (scheme == "javascript" || scheme == "vbscript")
    {
        return true;
    }
    // data: は <a href> 等のナビゲーションでは無条件危険扱い（text/html・script の実行面を断つ）。
    // 画像のプレースホルダは pika 生成の data: のみで、ユーザー入力の data: は通さない。
    if (scheme == "data")
    {
        return true;
    }
    return false;
}

bool is_external_url(std::string_view raw)
{
    std::string probe = normalize_url_scheme_probe(raw);
    auto colon = probe.find(':');
    if (colon != std::string::npos)
    {
        std::string scheme = probe.substr(0, colon);
        if (scheme == "http" || scheme == "https")
        {
            return true;
        }
    }
    // プロトコル相対 '//host'（スキーム省略で現在のスキームを継承＝外部）。
    // 先頭空白除去後の生値で判定する。
    std::size_t i = 0;
    while (i < raw.size() && static_cast<unsigned char>(raw[i]) <= 0x20)
    {
        ++i;
    }
    if (i + 1 < raw.size() && raw[i] == '/' && raw[i + 1] == '/')
    {
        return true;
    }
    return false;
}

} // namespace pika::core::render
