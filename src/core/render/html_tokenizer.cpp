#include "core/render/html_tokenizer.h"

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

std::string to_lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        out.push_back(ascii_lower(c));
    }
    return out;
}

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// タグ名先頭になりうる文字（ASCII 英字）。'<' の直後がこれでなければタグ開始とみなさない
// （'<' を地のテキストとして扱い、'3 < 5' のような誤検知を避ける）。
bool is_tagname_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_tagname_char(char c)
{
    return is_tagname_start(c) || (c >= '0' && c <= '9') || c == '-' || c == ':' || c == '_';
}

// script/style は生テキスト要素。中身の '<' を新たなタグとして解釈してはならない
// （</script> までを丸ごとテキストとして読み飛ばす。属性風の文字列を誤って属性化しない）。
bool is_rawtext_element(const std::string& lower_name)
{
    return lower_name == "script" || lower_name == "style";
}

} // namespace

std::vector<HtmlToken> tokenize_html(std::string_view html)
{
    std::vector<HtmlToken> tokens;
    const std::size_t n = html.size();
    std::size_t i = 0;

    auto push_text = [&](std::size_t start, std::size_t end) {
        if (end > start)
        {
            HtmlToken t;
            t.type = TokenType::Text;
            t.text.assign(html.substr(start, end - start));
            tokens.push_back(std::move(t));
        }
    };

    while (i < n)
    {
        if (html[i] != '<')
        {
            std::size_t start = i;
            while (i < n && html[i] != '<')
            {
                ++i;
            }
            push_text(start, i);
            continue;
        }

        // ここで html[i] == '<'。
        // コメント・宣言・処理命令（<!-- ... -->, <!doctype...>, <? ... ?>）を先に処理する。
        if (i + 1 < n && html[i + 1] == '!')
        {
            // <!-- ... --> コメント。終端 '-->' が無ければ末尾まで。
            if (i + 3 < n && html[i + 2] == '-' && html[i + 3] == '-')
            {
                std::size_t cstart = i + 4;
                std::size_t end = html.find("-->", cstart);
                HtmlToken t;
                t.type = TokenType::Comment;
                if (end == std::string_view::npos)
                {
                    t.text.assign(html.substr(cstart));
                    i = n;
                }
                else
                {
                    t.text.assign(html.substr(cstart, end - cstart));
                    i = end + 3;
                }
                tokens.push_back(std::move(t));
                continue;
            }
            // <!doctype ...> 等の宣言。'>' まで。
            std::size_t end = html.find('>', i + 2);
            HtmlToken t;
            t.type = TokenType::Doctype;
            if (end == std::string_view::npos)
            {
                t.text.assign(html.substr(i + 2));
                i = n;
            }
            else
            {
                t.text.assign(html.substr(i + 2, end - (i + 2)));
                i = end + 1;
            }
            tokens.push_back(std::move(t));
            continue;
        }

        // <? ... ?> 処理命令（XML PI）。'>' まで読み飛ばす（実行性のあるものではないが除去対象）。
        if (i + 1 < n && html[i + 1] == '?')
        {
            std::size_t end = html.find('>', i + 2);
            HtmlToken t;
            t.type = TokenType::Doctype;
            if (end == std::string_view::npos)
            {
                t.text.assign(html.substr(i + 1));
                i = n;
            }
            else
            {
                t.text.assign(html.substr(i + 1, end - (i + 1)));
                i = end + 1;
            }
            tokens.push_back(std::move(t));
            continue;
        }

        // 終了タグ </name>
        if (i + 1 < n && html[i + 1] == '/')
        {
            std::size_t p = i + 2;
            std::size_t name_start = p;
            while (p < n && is_tagname_char(html[p]))
            {
                ++p;
            }
            if (p == name_start)
            {
                // '</' の後がタグ名でない（'< /' 等）→ '<' を地のテキストとして消費。
                push_text(i, i + 1);
                ++i;
                continue;
            }
            std::string name = to_lower(html.substr(name_start, p - name_start));
            // '>' まで読み飛ばす（終了タグの属性は無視）。
            std::size_t end = html.find('>', p);
            HtmlToken t;
            t.type = TokenType::EndTag;
            t.name = std::move(name);
            i = (end == std::string_view::npos) ? n : end + 1;
            tokens.push_back(std::move(t));
            continue;
        }

        // 開始タグ <name ...>
        if (i + 1 < n && is_tagname_start(html[i + 1]))
        {
            std::size_t p = i + 1;
            std::size_t name_start = p;
            while (p < n && is_tagname_char(html[p]))
            {
                ++p;
            }
            std::string name = to_lower(html.substr(name_start, p - name_start));

            HtmlToken t;
            t.type = TokenType::StartTag;
            t.name = name;

            // 属性をパースする。'>' か '/>' まで。
            while (p < n)
            {
                while (p < n && is_space(html[p]))
                {
                    ++p;
                }
                if (p >= n)
                {
                    break;
                }
                if (html[p] == '>')
                {
                    ++p;
                    break;
                }
                if (html[p] == '/' && p + 1 < n && html[p + 1] == '>')
                {
                    t.self_closing = true;
                    p += 2;
                    break;
                }
                if (html[p] == '/')
                {
                    // 余った '/'（属性間の自己終了風）。スキップ。
                    ++p;
                    continue;
                }

                // 属性名
                std::size_t an_start = p;
                while (p < n && !is_space(html[p]) && html[p] != '=' && html[p] != '>' &&
                       html[p] != '/')
                {
                    ++p;
                }
                std::string aname = to_lower(html.substr(an_start, p - an_start));
                if (aname.empty())
                {
                    ++p; // 進行不能回避
                    continue;
                }

                Attribute attr;
                attr.name = std::move(aname);

                // '=' があれば値を読む。
                while (p < n && is_space(html[p]))
                {
                    ++p;
                }
                if (p < n && html[p] == '=')
                {
                    ++p;
                    while (p < n && is_space(html[p]))
                    {
                        ++p;
                    }
                    if (p < n && (html[p] == '"' || html[p] == '\''))
                    {
                        char quote = html[p];
                        ++p;
                        std::size_t v_start = p;
                        while (p < n && html[p] != quote)
                        {
                            ++p;
                        }
                        attr.value.assign(html.substr(v_start, p - v_start));
                        if (p < n)
                        {
                            ++p; // 終端クォート
                        }
                    }
                    else
                    {
                        std::size_t v_start = p;
                        while (p < n && !is_space(html[p]) && html[p] != '>')
                        {
                            ++p;
                        }
                        attr.value.assign(html.substr(v_start, p - v_start));
                    }
                }
                t.attrs.push_back(std::move(attr));
            }

            const bool rawtext = is_rawtext_element(name);
            tokens.push_back(std::move(t));
            i = p;

            // 生テキスト要素（script/style）は対応する終了タグまでを 1 つの Text として
            // 読み飛ばす（中身の '<' を新規タグと誤認しない）。終了タグはサニタイズ側が
            // 開始タグの除去で連鎖的に落とすため、ここでは中身を地テキスト扱いにする。
            if (rawtext && !tokens.back().self_closing)
            {
                std::string close = "</" + name;
                std::size_t rstart = i;
                std::size_t scan = i;
                std::size_t found = std::string_view::npos;
                while (scan < n)
                {
                    std::size_t cand = html.find('<', scan);
                    if (cand == std::string_view::npos)
                    {
                        break;
                    }
                    // 大文字小文字を無視して "</name" を照合。
                    if (cand + close.size() <= n)
                    {
                        bool match = true;
                        for (std::size_t k = 0; k < close.size(); ++k)
                        {
                            if (ascii_lower(html[cand + k]) != close[k])
                            {
                                match = false;
                                break;
                            }
                        }
                        if (match)
                        {
                            found = cand;
                            break;
                        }
                    }
                    scan = cand + 1;
                }
                std::size_t rend = (found == std::string_view::npos) ? n : found;
                push_text(rstart, rend);
                i = rend; // 終了タグ自体は次ループで通常処理（EndTag を出す）
            }
            continue;
        }

        // 上記いずれにも該当しない '<'（'<' の後が空白・数字など）は地のテキスト。
        push_text(i, i + 1);
        ++i;
    }

    return tokens;
}

} // namespace pika::core::render
