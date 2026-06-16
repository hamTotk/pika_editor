#include "controller/editor_view_model.h"

namespace pika::controller
{

namespace
{

EolMode to_eol_mode(util::Newline nl) noexcept
{
    switch (nl)
    {
    case util::Newline::Crlf:
        return EolMode::Crlf;
    case util::Newline::Mixed:
        return EolMode::Mixed;
    case util::Newline::Lf:
    case util::Newline::None:
    default:
        // 改行なし（None）は LF 既定で扱う（新規行を足すときの挿入改行。原文は空のまま）。
        return EolMode::Lf;
    }
}

} // namespace

EditorConfig make_editor_config(const core::settings::Settings& settings, util::Newline detected)
{
    EditorConfig cfg;
    // 0 幅タブは Scintilla で不正（桁送りが進まない）。安全側に 1 へ丸める。
    cfg.tab_width = settings.tab_width >= 1 ? static_cast<int>(settings.tab_width) : 1;
    // 「Tab キーで空白を挿入する」設定の論理反転がそのまま SCI_SETUSETABS。
    cfg.use_tabs = !settings.tab_inserts_spaces;
    cfg.word_wrap = settings.word_wrap;
    cfg.show_whitespace = settings.show_whitespace;
    cfg.eol_mode = to_eol_mode(detected);
    cfg.read_only = false;
    return cfg;
}

std::string tab_title_for_path(std::string_view absolute_path)
{
    if (absolute_path.empty())
    {
        return std::string();
    }
    // 末尾区切りを剥がしてから最後のセグメントを取り出す（"C:/a/b/" → "b"）。
    std::size_t end = absolute_path.size();
    while (end > 0 && (absolute_path[end - 1] == '/' || absolute_path[end - 1] == '\\'))
    {
        --end;
    }
    if (end == 0)
    {
        // 区切りだけ（"/" 等）。表示名は持たない。
        return std::string();
    }
    std::string_view trimmed = absolute_path.substr(0, end);
    const std::size_t sep = trimmed.find_last_of("/\\");
    std::string_view name = (sep == std::string_view::npos) ? trimmed : trimmed.substr(sep + 1);
    return std::string(name);
}

} // namespace pika::controller
