#include "controller/open_view_model.h"

#include "controller/degrade_model.h"
#include "controller/tree_view_model.h"
#include "util/binary_detect.h"
#include "util/image_header.h"

#include <cctype>

namespace pika::controller
{

namespace
{

// 末尾の拡張子（小文字・ドットなし）を取り出す。先頭ドットのみ（".gitignore"）は拡張子なし扱い。
std::string ext_lower(std::string_view name_or_path)
{
    const std::size_t dot = name_or_path.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0)
    {
        return {};
    }
    // パス区切りより後ろの '.' のみを拡張子と見なす（"a.b/c" のような無拡張を誤検出しない）。
    const std::size_t slash = name_or_path.find_last_of("/\\");
    if (slash != std::string_view::npos && dot < slash)
    {
        return {};
    }
    std::string out;
    for (std::size_t i = dot + 1; i < name_or_path.size(); ++i)
    {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(name_or_path[i]))));
    }
    return out;
}

// ラスター画像か（IconCategory::Image かつ svg 以外）。svg はベクタでラスタ簡易ビューの対象外。
bool is_raster_image(std::string_view name_or_path)
{
    if (classify_icon(name_or_path) != IconCategory::Image)
    {
        return false;
    }
    return ext_lower(name_or_path) != "svg";
}

} // namespace

OpenViewResult resolve_open_view(const OpenViewInput& in)
{
    OpenViewResult out;

    // 1. 画像拡張子 → 画像経路（ヘッダ寸法で総ピクセル数ガードを掛ける。要件2.2/12.2）。
    if (is_raster_image(in.name_or_path))
    {
        const util::ImageDimensions dim = util::parse_image_dimensions(in.head);
        if (dim.known)
        {
            out.pixel_count = dim.pixel_count();
            // 総ピクセル数ガードは degrade_model（系統A・gtest 済み）に委ねる。
            DegradeInput d;
            d.is_image = true;
            d.pixel_count = dim.pixel_count();
            d.max_pixels = in.max_pixels;
            const DegradeOutcome r = resolve_degrade(d);
            out.view =
                (r.kind == DegradeKind::ImageTooLarge) ? OpenView::ImageTooLarge : OpenView::Image;
            return out;
        }
        // ヘッダ寸法を取れない（JPEG/WEBP/ICO 等）。フォールバック: 極端に大きいファイルは
        // デコード爆発を避けてガード扱い、そうでなければ wxImage 通常デコードに委ねる（Image）。
        out.view =
            (in.file_size > in.fallback_max_bytes) ? OpenView::ImageTooLarge : OpenView::Image;
        return out;
    }

    // 2. 画像でなく非テキスト（NUL/制御文字比率）→ 非対応ビュー（I9。要件12.2）。
    if (util::looks_binary(in.head))
    {
        out.view = OpenView::Unsupported;
        return out;
    }

    // 3. それ以外はテキスト（EditorPanel・従来経路）。
    out.view = OpenView::Text;
    return out;
}

} // namespace pika::controller
