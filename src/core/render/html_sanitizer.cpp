#include "core/render/html_sanitizer.h"

#include "core/render/html_tokenizer.h"
#include "core/render/url_classifier.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace pika::core::render
{

namespace
{

// 許可タグ（ホワイトリスト）。Markdown が生成しうる要素 ＋ GFM（table 系・チェックボックス）＋
// インライン SVG の安全な描画要素に限る。ここに無いタグは「タグだけ落として中身は残す」。
const std::unordered_set<std::string>& allowed_tags()
{
    static const std::unordered_set<std::string> s = {
        // ブロック・見出し・段落
        "p", "div", "span", "br", "hr", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "pre",
        "code", "kbd", "samp", "var",
        // リスト
        "ul", "ol", "li", "dl", "dt", "dd",
        // 強調・インライン
        "em", "strong", "b", "i", "u", "s", "strike", "del", "ins", "mark", "sub", "sup", "small",
        "abbr", "cite", "q", "wbr",
        // テーブル（GFM）
        "table", "thead", "tbody", "tfoot", "tr", "th", "td", "caption", "colgroup", "col",
        // リンク・画像
        "a", "img", "figure", "figcaption",
        // インライン SVG（安全な描画要素のみ。script/foreignObject は別途禁止）
        "svg", "g", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon", "text",
        "tspan", "defs", "lineargradient", "radialgradient", "stop", "use", "symbol", "marker",
        "clippath", "title", "desc", "details", "summary"};
    return s;
}

// 中身ごと丸ごと落とす危険タグ（コンテナ）。開始から対応する終了まで子孫も出力しない。
// design.md 6章 / 要件6.2 の明示対象 ＋ インライン SVG 無害化対象（foreignObject）。
// 注: input/embed/base/link/meta/frame は void 要素（終了タグを持たない）ため、ここではなく
// forbidden_void_tags で「タグだけ落とす」扱いにする（誤って後続を全抑制しないため）。
const std::unordered_set<std::string>& forbidden_subtree_tags()
{
    // 注: title/marquee は subtree 抑制しない（title は SVG の許可タグと衝突するため許可側で扱い、
    // marquee は非危険＝未知タグ扱いでタグだけ落とす）。
    static const std::unordered_set<std::string> s = {
        "script", "iframe", "object", "style",    "template", "noscript", "form",
        "button", "select", "option", "textarea", "applet",   "frameset", "foreignobject"};
    return s;
}

// 危険な void/置換要素（終了タグを持たない、または持っても無意味）。タグ自体を落とすが、
// サブツリー抑制はしない（後続の安全なテキスト・要素を巻き込まない）。
const std::unordered_set<std::string>& forbidden_void_tags()
{
    static const std::unordered_set<std::string> s = {"embed", "base", "link", "meta", "frame"};
    return s;
}

// タグごとの許可属性。共通許可（class/id/title 等）に加え、タグ固有を足す。
bool is_globally_allowed_attr(const std::string& name)
{
    // on* は問答無用で禁止（イベントハンドラ）。
    if (name.size() >= 2 && name[0] == 'o' && name[1] == 'n')
    {
        return false;
    }
    return name == "class" || name == "id" || name == "title" || name == "lang" || name == "dir" ||
           name == "role" || name == "align" || name == "colspan" || name == "rowspan" ||
           name == "start" || name == "type" || name == "open" || name == "datetime" ||
           name == "cite";
}

// URL を値に取る属性（スキーム検査の対象）。
bool is_url_attr(const std::string& name)
{
    return name == "href" || name == "src" || name == "xlink:href" || name == "poster" ||
           name == "action" || name == "formaction" || name == "data" || name == "background";
}

// SVG の許可属性（描画系）。多いため代表的な安全属性を許可し、それ以外は落とす。
bool is_svg_allowed_attr(const std::string& name)
{
    static const std::unordered_set<std::string> s = {"d",
                                                      "fill",
                                                      "stroke",
                                                      "stroke-width",
                                                      "stroke-linecap",
                                                      "stroke-linejoin",
                                                      "stroke-dasharray",
                                                      "cx",
                                                      "cy",
                                                      "r",
                                                      "rx",
                                                      "ry",
                                                      "x",
                                                      "y",
                                                      "x1",
                                                      "y1",
                                                      "x2",
                                                      "y2",
                                                      "width",
                                                      "height",
                                                      "viewbox",
                                                      "points",
                                                      "transform",
                                                      "opacity",
                                                      "fill-opacity",
                                                      "stroke-opacity",
                                                      "offset",
                                                      "stop-color",
                                                      "stop-opacity",
                                                      "gradientunits",
                                                      "gradienttransform",
                                                      "patternunits",
                                                      "clip-path",
                                                      "clippathunits",
                                                      "marker-end",
                                                      "marker-start",
                                                      "marker-mid",
                                                      "preserveaspectratio",
                                                      "xmlns",
                                                      "xmlns:xlink",
                                                      "text-anchor",
                                                      "font-size",
                                                      "font-family",
                                                      "dy",
                                                      "dx",
                                                      "version"};
    return s.count(name) != 0;
}

// 出力テキストの HTML エスケープ（< > & " を実体参照へ）。
void append_escaped_text(std::string& out, std::string_view text)
{
    for (char c : text)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
}

// 属性値のエスケープ（ダブルクォート区切り前提。" と & と < を実体参照へ）。
void append_escaped_attr_value(std::string& out, std::string_view value)
{
    for (char c : value)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
}

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

// style 属性値から危険な CSS を遮断する。url() と @import と expression() を含むなら
// その style 属性自体を落とす（部分的に残すと回避余地が増えるため、丸ごと捨てる安全側）。
// design.md 6章「CSS url()/@import の遮断」。
bool is_css_safe(std::string_view css)
{
    if (icontains(css, "url(") || icontains(css, "@import") || icontains(css, "expression(") ||
        icontains(css, "javascript:") || icontains(css, "/*"))
    {
        return false;
    }
    return true;
}

bool attr_equals(const std::vector<Attribute>& attrs, const std::string& name,
                 const std::string& value)
{
    for (const auto& a : attrs)
    {
        if (a.name == name)
        {
            std::string v;
            for (char c : a.value)
            {
                v.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
            }
            return v == value;
        }
    }
    return false;
}

bool has_attr(const std::vector<Attribute>& attrs, const std::string& name)
{
    for (const auto& a : attrs)
    {
        if (a.name == name)
        {
            return true;
        }
    }
    return false;
}

// GFM タスクリストのチェックボックスか（md4c 出力は disabled な checkbox）。
// 無効化チェックボックスは無害であり、要件6.2「タスクリスト」描画に必須なので、この形だけ通す。
// 入力由来の任意 <input> は通さない（type=checkbox かつ disabled の厳密一致のみ）。
bool is_gfm_task_checkbox(const HtmlToken& tok)
{
    return tok.name == "input" && attr_equals(tok.attrs, "type", "checkbox") &&
           has_attr(tok.attrs, "disabled");
}

} // namespace

std::string sanitize_html(std::string_view html)
{
    const auto tokens = tokenize_html(html);
    std::string out;
    out.reserve(html.size());

    // 危険タグの中身を読み飛ばすための「抑制スタック」。forbidden サブツリーに入ったら、
    // その閉じタグまで出力しない（ネストにも対応する）。
    int suppress_depth = 0;
    std::string suppress_tag; // 抑制を開始したタグ名（対応する終了で解除）

    for (const auto& tok : tokens)
    {
        // 抑制中：対応する終了タグでのみ解除し、それ以外は一切出力しない。
        if (suppress_depth > 0)
        {
            if (tok.type == TokenType::StartTag && tok.name == suppress_tag && !tok.self_closing)
            {
                ++suppress_depth;
            }
            else if (tok.type == TokenType::EndTag && tok.name == suppress_tag)
            {
                --suppress_depth;
            }
            continue;
        }

        switch (tok.type)
        {
        case TokenType::Text:
            append_escaped_text(out, tok.text);
            break;

        case TokenType::Comment:
        case TokenType::Doctype:
            // コメント・宣言・処理命令は出力しない（条件付きコメント等の抜け穴を作らない）。
            break;

        case TokenType::EndTag:
            // 許可タグの終了のみ出力（void 要素は終了タグを持たないが、来ても無害）。
            if (allowed_tags().count(tok.name) != 0)
            {
                out += "</";
                out += tok.name;
                out += ">";
            }
            break;

        case TokenType::StartTag: {
            // 危険サブツリー：開始から終了まで中身ごと落とす。
            if (forbidden_subtree_tags().count(tok.name) != 0)
            {
                if (!tok.self_closing)
                {
                    suppress_depth = 1;
                    suppress_tag = tok.name;
                }
                break;
            }
            // GFM タスクリストの無効化チェックボックス（要件6.2）。無害な固定形だけを出力する。
            if (is_gfm_task_checkbox(tok))
            {
                out += "<input type=\"checkbox\" disabled";
                if (has_attr(tok.attrs, "checked"))
                {
                    out += " checked";
                }
                out += " />";
                break;
            }
            // 危険な void 要素（embed/base/link/meta/frame）：タグだけ落とし中身は巻き込まない。
            if (forbidden_void_tags().count(tok.name) != 0)
            {
                break;
            }
            // 未知タグ：タグは落とすが中身（後続テキスト）は残す（黙って消さない）。
            if (allowed_tags().count(tok.name) == 0)
            {
                break;
            }

            // 許可タグ：属性をホワイトリストで濾過して出力する。
            // SVG 描画属性はタグ別に厳密化せず描画系の安全名のみ許可する（タグ別判定は YAGNI）。
            out += "<";
            out += tok.name;
            for (const auto& attr : tok.attrs)
            {
                const std::string& name = attr.name;

                // on* イベント属性は常に除去。
                if (name.size() >= 2 && name[0] == 'o' && name[1] == 'n')
                {
                    continue;
                }
                // style 属性は CSS を検査し、危険なら丸ごと落とす。
                if (name == "style")
                {
                    if (is_css_safe(attr.value))
                    {
                        out += " style=\"";
                        append_escaped_attr_value(out, attr.value);
                        out += "\"";
                    }
                    continue;
                }
                // URL 属性は危険スキームなら除去。
                if (is_url_attr(name))
                {
                    if (is_dangerous_url(attr.value))
                    {
                        continue;
                    }
                    out += " ";
                    out += name;
                    out += "=\"";
                    append_escaped_attr_value(out, attr.value);
                    out += "\"";
                    continue;
                }
                // img の alt、a の name 等の安全な汎用属性 ＋ SVG 描画属性を許可。
                if (is_globally_allowed_attr(name) || name == "alt" || name == "name" ||
                    name == "width" || name == "height" || name == "scope" ||
                    is_svg_allowed_attr(name))
                {
                    out += " ";
                    out += name;
                    out += "=\"";
                    append_escaped_attr_value(out, attr.value);
                    out += "\"";
                    continue;
                }
                // それ以外は除去（data-* も既定で落とす：pika 生成の data-line は別経路）。
            }
            if (tok.self_closing)
            {
                out += " /";
            }
            out += ">";
            break;
        }
        }
    }

    return out;
}

} // namespace pika::core::render
