#include "core/render/render_guard.h"

#include <limits>

namespace pika::core::render
{

namespace
{

// width*height がオーバーフローせず、上限以下かを判定する。
// 0 次元（width または height が 0）は描画対象として不正＝不可（安全側）。
// 乗算オーバーフローは「巨大すぎて算出不能」＝上限超過として扱う。
bool pixels_within(std::uint64_t width, std::uint64_t height, std::uint64_t limit,
                   std::uint64_t& out_pixels)
{
    if (width == 0 || height == 0)
    {
        out_pixels = 0;
        return false;
    }
    if (width > std::numeric_limits<std::uint64_t>::max() / height)
    {
        out_pixels = std::numeric_limits<std::uint64_t>::max();
        return false;
    }
    out_pixels = width * height;
    return out_pixels <= limit;
}

} // namespace

RenderGuardVerdict guard_image(std::uint64_t width, std::uint64_t height,
                               const RenderGuardLimits& limits)
{
    std::uint64_t pixels = 0;
    if (!pixels_within(width, height, limits.max_image_pixels, pixels))
    {
        return RenderGuardVerdict{false, "image-pixels-exceeded"};
    }
    return RenderGuardVerdict{true, ""};
}

RenderGuardVerdict guard_svg(std::uint64_t width, std::uint64_t height, std::size_t element_count,
                             const RenderGuardLimits& limits)
{
    std::uint64_t pixels = 0;
    if (!pixels_within(width, height, limits.max_svg_pixels, pixels))
    {
        return RenderGuardVerdict{false, "svg-pixels-exceeded"};
    }
    if (element_count > limits.max_svg_elements)
    {
        return RenderGuardVerdict{false, "svg-elements-exceeded"};
    }
    return RenderGuardVerdict{true, ""};
}

RenderGuardVerdict guard_html(std::size_t element_count, std::size_t nest_depth,
                              const RenderGuardLimits& limits)
{
    if (element_count > limits.max_html_elements)
    {
        return RenderGuardVerdict{false, "html-elements-exceeded"};
    }
    if (nest_depth > limits.max_html_nest_depth)
    {
        return RenderGuardVerdict{false, "html-nest-depth-exceeded"};
    }
    return RenderGuardVerdict{true, ""};
}

} // namespace pika::core::render
