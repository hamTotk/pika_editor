#include "core/render/html_inspector.h"

#include "core/render/html_tokenizer.h"
#include "core/render/url_classifier.h"

namespace pika::core::render
{

namespace
{

// 値に部分文字列 needle（小文字想定）を含むか、大文字小文字を無視して判定する。
bool icontains(std::string_view hay, std::string_view needle)
{
    if (needle.empty() || needle.size() > hay.size())
    {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
    {
        bool match = true;
        for (std::size_t k = 0; k < needle.size(); ++k)
        {
            char c = hay[i + k];
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != needle[k])
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}

// 外部リソース参照を持ちうる属性（要件6.3：画像・CSS・フォント・preload 等）。
bool is_resource_attr(const std::string& name)
{
    return name == "src" || name == "href" || name == "srcset" || name == "poster" ||
           name == "data" || name == "background" || name == "formaction" || name == "action";
}

} // namespace

InspectionResult inspect_html(std::string_view html)
{
    InspectionResult r;
    const auto tokens = tokenize_html(html);

    for (const auto& tok : tokens)
    {
        if (tok.type != TokenType::StartTag)
        {
            continue;
        }

        // <script> は JS 依存（要件6.3）。
        if (tok.name == "script")
        {
            r.has_script = true;
        }

        for (const auto& attr : tok.attrs)
        {
            // Tailwind CDN 参照（cdn.tailwindcss.com 等）。script src でも link href でも拾う。
            if (is_resource_attr(attr.name) && icontains(attr.value, "tailwindcss.com"))
            {
                r.has_tailwind_cdn = true;
            }
            // 外部 http(s) リソース参照の検知（要件2.4・6.2・6.3）。
            if (is_resource_attr(attr.name) && is_external_url(attr.value))
            {
                r.has_external_resource = true;
            }
            // style 属性・<style> 内の url(http...) も外部参照になりうる。
            if (attr.name == "style" && icontains(attr.value, "url(") &&
                (icontains(attr.value, "http://") || icontains(attr.value, "https://") ||
                 icontains(attr.value, "//")))
            {
                // url(//host) のプロトコル相対も外部とみなす（保守的）。
                if (icontains(attr.value, "http") || icontains(attr.value, "url(//"))
                {
                    r.has_external_resource = true;
                }
            }
        }
    }

    // <style> ブロックや地テキスト内の @import url(http...) も外部参照。
    // tokenizer は script/style の中身を Text として出すため、Text を走査する。
    for (const auto& tok : tokens)
    {
        if (tok.type != TokenType::Text)
        {
            continue;
        }
        if ((icontains(tok.text, "@import") || icontains(tok.text, "url(")) &&
            (icontains(tok.text, "http://") || icontains(tok.text, "https://")))
        {
            r.has_external_resource = true;
        }
    }

    return r;
}

} // namespace pika::core::render
