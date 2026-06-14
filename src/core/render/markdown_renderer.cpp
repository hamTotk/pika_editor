#include "core/render/markdown_renderer.h"

#include "core/render/html_sanitizer.h"

#include <md4c-html.h>

namespace pika::core::render
{

namespace
{

// md_html のチャンクコールバック。userdata に渡した std::string へ追記する。
void collect_chunk(const MD_CHAR* chunk, MD_SIZE size, void* userdata)
{
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(chunk, size);
}

} // namespace

pika::util::Result<std::string> render_markdown(std::string_view markdown)
{
    std::string raw_html;
    raw_html.reserve(markdown.size() + markdown.size() / 2);

    // GFM ダイアレクト（テーブル・タスクリスト・打消し線・自動リンク）。
    // VERBATIM_ENTITIES で md4c に実体参照を素通しさせ、エスケープはサニタイザに一元化する。
    const unsigned parser_flags = MD_DIALECT_GITHUB;
    const unsigned renderer_flags = 0;

    const int rc = md_html(markdown.data(), static_cast<MD_SIZE>(markdown.size()), &collect_chunk,
                           &raw_html, parser_flags, renderer_flags);
    if (rc != 0)
    {
        return pika::util::Result<std::string>::err(pika::util::ErrorCode::Unknown,
                                                    "md4c conversion failed");
    }

    // md4c 出力は Markdown 内 raw HTML を含みうる。必ずサニタイズしてから返す
    // （サニタイズ前 HTML を外へ出さない＝XSS 境界を一元化する）。
    return pika::util::Result<std::string>::ok(sanitize_html(raw_html));
}

} // namespace pika::core::render
