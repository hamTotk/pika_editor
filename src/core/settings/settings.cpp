#include "core/settings/settings.h"

#include <algorithm>
#include <cctype>

#include <toml.hpp>

namespace pika::core::settings
{

namespace
{

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// TOML テーブルから key を辿る（"a.b" のドット区切りも 1 段ネストとして解決する）。
// 見つかった node を返す。無ければ nullptr。型チェックは呼び出し側で行う。
const toml::value* find_node(const toml::value& root, std::string_view dotted_key)
{
    const toml::value* cur = &root;
    std::size_t start = 0;
    while (start <= dotted_key.size())
    {
        std::size_t dot = dotted_key.find('.', start);
        std::string_view seg = dotted_key.substr(
            start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (!cur->is_table())
        {
            return nullptr;
        }
        const auto& tbl = cur->as_table();
        auto it = tbl.find(std::string(seg));
        if (it == tbl.end())
        {
            return nullptr;
        }
        cur = &it->second;
        if (dot == std::string_view::npos)
        {
            break;
        }
        start = dot + 1;
    }
    return cur;
}

// bool を取り出す。型違いなら fallback を維持し warnings にキーを積む。
void load_bool(const toml::value& root, std::string_view key, bool& out,
               std::vector<std::string>& warnings)
{
    const toml::value* node = find_node(root, key);
    if (node == nullptr)
    {
        return; // 未指定は既定維持（警告しない）
    }
    if (!node->is_boolean())
    {
        warnings.emplace_back(std::string(key));
        return;
    }
    out = node->as_boolean();
}

// 符号なし整数を取り出す。型違い・負値・範囲外（min/max）なら fallback 維持＋警告。
void load_uint(const toml::value& root, std::string_view key, std::uint64_t& out,
               std::vector<std::string>& warnings, std::uint64_t min_val, std::uint64_t max_val)
{
    const toml::value* node = find_node(root, key);
    if (node == nullptr)
    {
        return;
    }
    if (!node->is_integer())
    {
        warnings.emplace_back(std::string(key));
        return;
    }
    std::int64_t v = node->as_integer();
    if (v < 0)
    {
        warnings.emplace_back(std::string(key));
        return;
    }
    auto uv = static_cast<std::uint64_t>(v);
    if (uv < min_val || uv > max_val)
    {
        warnings.emplace_back(std::string(key));
        return;
    }
    out = uv;
}

// 文字列を取り出す。型違いなら fallback 維持＋警告。空文字許容は許可しない（既定維持）。
void load_string(const toml::value& root, std::string_view key, std::string& out,
                 std::vector<std::string>& warnings)
{
    const toml::value* node = find_node(root, key);
    if (node == nullptr)
    {
        return;
    }
    if (!node->is_string())
    {
        warnings.emplace_back(std::string(key));
        return;
    }
    const std::string& s = node->as_string();
    if (s.empty())
    {
        warnings.emplace_back(std::string(key));
        return;
    }
    out = s;
}

// 文字列配列を取り出す。型違い（配列でない・要素に非文字列）なら fallback 維持＋警告。
void load_string_list(const toml::value& root, std::string_view key, std::vector<std::string>& out,
                      std::vector<std::string>& warnings)
{
    const toml::value* node = find_node(root, key);
    if (node == nullptr)
    {
        return;
    }
    if (!node->is_array())
    {
        warnings.emplace_back(std::string(key));
        return;
    }
    std::vector<std::string> tmp;
    for (const auto& el : node->as_array())
    {
        if (!el.is_string())
        {
            warnings.emplace_back(std::string(key));
            return; // 1 要素でも不正なら全体を既定維持（部分採用しない）
        }
        tmp.push_back(el.as_string());
    }
    out = std::move(tmp);
}

// 列挙系（文字列→列挙）の汎用ロード。許可語に無ければ fallback 維持＋警告。
template <typename Enum>
void load_enum(const toml::value& root, std::string_view key, Enum& out,
               std::vector<std::string>& warnings,
               const std::vector<std::pair<const char*, Enum>>& choices)
{
    const toml::value* node = find_node(root, key);
    if (node == nullptr)
    {
        return;
    }
    if (!node->is_string())
    {
        warnings.emplace_back(std::string(key));
        return;
    }
    std::string v = to_lower(node->as_string());
    for (const auto& [name, val] : choices)
    {
        if (v == name)
        {
            out = val;
            return;
        }
    }
    warnings.emplace_back(std::string(key));
}

} // namespace

Settings default_settings()
{
    return Settings{};
}

LoadResult load_settings(std::string_view toml_text, const Settings& previous)
{
    LoadResult result;

    // TOML パース。破損（保存途中の不完全な TOML 等）は例外で来るため捕捉し、直前の有効値を維持する
    // （要件10.3「既定値への全戻しでちらつかせない」）。
    // parse_str は文字列を TOML 内容として解釈する（toml::parse(std::string) はファイル名扱いになる
    // ため使わない）。
    toml::value root;
    try
    {
        root = toml::parse_str(std::string(toml_text));
    }
    catch (const std::exception&)
    {
        result.settings = previous;
        result.parse_ok = false;
        return result;
    }

    // 構文 OK。既定から開始し、指定された値を検証しながら上書きする（不正値は既定のまま＋警告）。
    Settings s = default_settings();
    auto& w = result.warnings;

    load_string_list(root, "exclude", s.exclude, w);
    load_string_list(root, "snapshot.sensitivePatterns", s.sensitive_patterns, w);

    // 巨大ファイル閾値は 0 を許さない（1 バイト以上）。段階1<段階2 の整合は別途下で点検する。
    load_uint(root, "bigFile.stage1Bytes", s.big_file_stage1_bytes, w, 1, UINT64_MAX);
    load_uint(root, "bigFile.stage2Bytes", s.big_file_stage2_bytes, w, 1, UINT64_MAX);
    load_uint(root, "maxLineLength", s.max_line_length, w, 1, UINT64_MAX);

    load_uint(root, "render.maxImagePx", s.render_max_image_px, w, 1, UINT64_MAX);
    load_uint(root, "render.maxSvgPx", s.render_max_svg_px, w, 1, UINT64_MAX);
    load_uint(root, "render.maxHtmlElements", s.render_max_html_elements, w, 1, UINT64_MAX);

    load_bool(root, "allowRemoteResources", s.allow_remote_resources, w);

    load_enum<ViewMode>(root, "defaultMode", s.default_mode, w,
                        {{"preview", ViewMode::Preview},
                         {"split", ViewMode::Split},
                         {"source", ViewMode::Source},
                         {"diff", ViewMode::Diff}});
    load_enum<NewFileEncoding>(root, "newFileEncoding", s.new_file_encoding, w,
                               {{"utf-8", NewFileEncoding::Utf8},
                                {"utf8", NewFileEncoding::Utf8},
                                {"utf-8-bom", NewFileEncoding::Utf8Bom},
                                {"utf8bom", NewFileEncoding::Utf8Bom},
                                {"shift_jis", NewFileEncoding::ShiftJis},
                                {"shift-jis", NewFileEncoding::ShiftJis}});
    load_enum<Newline>(root, "newFileNewline", s.new_file_newline, w,
                       {{"lf", Newline::Lf}, {"crlf", Newline::Crlf}});
    load_enum<Theme>(root, "theme", s.theme, w,
                     {{"system", Theme::System}, {"light", Theme::Light}, {"dark", Theme::Dark}});

    load_bool(root, "wordWrap", s.word_wrap, w);
    // タブ幅は 1〜16 に制限（極端値はエディタ表示を壊す）。
    load_uint(root, "tabWidth", s.tab_width, w, 1, 16);
    load_bool(root, "tabInsertsSpaces", s.tab_inserts_spaces, w);
    load_bool(root, "showWhitespace", s.show_whitespace, w);

    load_string(root, "fontFamily", s.font_family, w);
    // フォントサイズは 6〜72pt。
    load_uint(root, "fontSize", s.font_size, w, 6, 72);

    load_uint(root, "snapshot.capacityBytes", s.snapshot_capacity_bytes, w, 1, UINT64_MAX);

    load_bool(root, "unread.fullHashOnStartup", s.unread_full_hash_on_startup, w);
    // ポーリング間隔は 1〜3600 秒。
    load_uint(root, "pollIntervalSeconds", s.poll_interval_seconds, w, 1, 3600);

    load_bool(root, "feature.mermaid", s.feature_mermaid, w);
    load_bool(root, "feature.math", s.feature_math, w);
    load_bool(root, "feature.codeHighlight", s.feature_code_highlight, w);

    // 段階1 が段階2 以上なら整合崩れ。段階2 を既定へ戻して警告する（巨大ファイル判定を壊さない）。
    if (s.big_file_stage1_bytes >= s.big_file_stage2_bytes)
    {
        s.big_file_stage2_bytes = default_settings().big_file_stage2_bytes;
        if (s.big_file_stage1_bytes >= s.big_file_stage2_bytes)
        {
            s.big_file_stage1_bytes = default_settings().big_file_stage1_bytes;
        }
        w.emplace_back("bigFile.stage2Bytes");
    }

    result.settings = std::move(s);
    result.parse_ok = true;
    return result;
}

LoadResult load_settings(std::string_view toml_text)
{
    return load_settings(toml_text, default_settings());
}

} // namespace pika::core::settings
