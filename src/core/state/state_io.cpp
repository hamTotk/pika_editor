#include "core/state/state_io.h"

#include "core/snapshot/json_lite.h"
#include "util/atomic_file.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace pika::core::state
{

namespace fs = std::filesystem;
namespace json = pika::core::snapshot::json;
using pika::util::ErrorCode;
using pika::util::Result;

namespace
{

// 「最近使った」は最大 kRecentLimit 件まで（要件10.2）。先頭が新しい順という前提で先頭を残す。
std::vector<std::string> clamp_recent(const std::vector<std::string>& items)
{
    if (items.size() <= kRecentLimit)
    {
        return items;
    }
    return std::vector<std::string>(items.begin(), items.begin() + kRecentLimit);
}

json::Value string_array(const std::vector<std::string>& items)
{
    json::Value arr = json::Value::array();
    for (const auto& s : items)
    {
        arr.push_back(json::Value::str(s));
    }
    return arr;
}

void read_string_array(const json::Value* node, std::vector<std::string>& out)
{
    if (node == nullptr || !node->is_array())
    {
        return;
    }
    for (const auto& item : node->items())
    {
        out.push_back(item.as_string());
    }
}

} // namespace

std::string serialize_state(const AppState& state)
{
    json::Value root = json::Value::object();
    root.set("version", json::Value::integer(state.version));

    json::Value window = json::Value::object();
    window.set("x", json::Value::integer(state.window.x));
    window.set("y", json::Value::integer(state.window.y));
    window.set("width", json::Value::integer(state.window.width));
    window.set("height", json::Value::integer(state.window.height));
    window.set("maximized", json::Value::boolean(state.window.maximized));
    root.set("window", std::move(window));

    root.set("lastWorkspace", json::Value::str(state.last_workspace));

    json::Value tabs = json::Value::array();
    for (const auto& t : state.tabs)
    {
        json::Value jt = json::Value::object();
        jt.set("path", json::Value::str(t.path));
        jt.set("caret", json::Value::integer(t.caret));
        jt.set("scroll", json::Value::integer(t.scroll));
        jt.set("mode", json::Value::str(t.mode));
        jt.set("previewScroll", json::Value::integer(t.preview_scroll));
        tabs.push_back(std::move(jt));
    }
    root.set("tabs", std::move(tabs));

    root.set("activeTab", json::Value::integer(state.active_tab));
    root.set("treeExpanded", string_array(state.tree_expanded));

    // modeByType は拡張子→モードの順序付き写像。json_lite はオブジェクトのメンバ列挙 getter を
    // 持たないため、読み戻せるよう {ext, mode} オブジェクトの配列で持つ（挿入順を保つ）。
    json::Value mode_by_type = json::Value::array();
    for (const auto& [ext, mode] : state.mode_by_type)
    {
        json::Value pair = json::Value::object();
        pair.set("ext", json::Value::str(ext));
        pair.set("mode", json::Value::str(mode));
        mode_by_type.push_back(std::move(pair));
    }
    root.set("modeByType", std::move(mode_by_type));

    json::Value theme = json::Value::object();
    theme.set("current", json::Value::str(state.theme_current));
    root.set("theme", std::move(theme));

    json::Value recent = json::Value::object();
    recent.set("files", string_array(clamp_recent(state.recent.files)));
    recent.set("folders", string_array(clamp_recent(state.recent.folders)));
    root.set("recent", std::move(recent));

    return root.dump();
}

Result<AppState> parse_state(std::string_view text)
{
    json::Value root;
    if (!json::parse(text, root) || !root.is_object())
    {
        return Result<AppState>::err(ErrorCode::Io, "state.json のパースに失敗しました");
    }

    AppState state;
    const json::Value* ver = root.get("version");
    if (ver == nullptr)
    {
        return Result<AppState>::err(ErrorCode::Io, "state.json に version がありません");
    }
    state.version = static_cast<int>(ver->as_int(0));
    // 未知（新しい）version は読み込まず安全側に倒す（K2。旧版が新版状態を破壊しない）。
    if (state.version > kStateVersion)
    {
        return Result<AppState>::err(ErrorCode::Unsupported,
                                     "state.json の version が新しすぎます");
    }

    if (const auto* w = root.get("window"); w != nullptr && w->is_object())
    {
        if (const auto* p = w->get("x"))
        {
            state.window.x = p->as_int();
        }
        if (const auto* p = w->get("y"))
        {
            state.window.y = p->as_int();
        }
        if (const auto* p = w->get("width"))
        {
            state.window.width = p->as_int();
        }
        if (const auto* p = w->get("height"))
        {
            state.window.height = p->as_int();
        }
        if (const auto* p = w->get("maximized"))
        {
            state.window.maximized = p->as_bool();
        }
    }

    if (const auto* p = root.get("lastWorkspace"))
    {
        state.last_workspace = p->as_string();
    }

    if (const auto* tabs = root.get("tabs"); tabs != nullptr && tabs->is_array())
    {
        for (const auto& jt : tabs->items())
        {
            if (!jt.is_object())
            {
                continue;
            }
            TabState t;
            if (const auto* p = jt.get("path"))
            {
                t.path = p->as_string();
            }
            if (const auto* p = jt.get("caret"))
            {
                t.caret = p->as_int();
            }
            if (const auto* p = jt.get("scroll"))
            {
                t.scroll = p->as_int();
            }
            if (const auto* p = jt.get("mode"))
            {
                t.mode = p->as_string();
            }
            if (const auto* p = jt.get("previewScroll"))
            {
                t.preview_scroll = p->as_int();
            }
            state.tabs.push_back(std::move(t));
        }
    }

    if (const auto* p = root.get("activeTab"))
    {
        state.active_tab = p->as_int(-1);
    }

    read_string_array(root.get("treeExpanded"), state.tree_expanded);

    if (const auto* m = root.get("modeByType"); m != nullptr && m->is_array())
    {
        for (const auto& jp : m->items())
        {
            if (!jp.is_object())
            {
                continue;
            }
            std::string ext;
            std::string mode;
            if (const auto* p = jp.get("ext"))
            {
                ext = p->as_string();
            }
            if (const auto* p = jp.get("mode"))
            {
                mode = p->as_string();
            }
            state.mode_by_type.emplace_back(std::move(ext), std::move(mode));
        }
    }

    if (const auto* theme = root.get("theme"); theme != nullptr && theme->is_object())
    {
        if (const auto* p = theme->get("current"))
        {
            state.theme_current = p->as_string();
        }
    }

    if (const auto* recent = root.get("recent"); recent != nullptr && recent->is_object())
    {
        read_string_array(recent->get("files"), state.recent.files);
        read_string_array(recent->get("folders"), state.recent.folders);
        state.recent.files = clamp_recent(state.recent.files);
        state.recent.folders = clamp_recent(state.recent.folders);
    }

    return Result<AppState>::ok(std::move(state));
}

Result<AppState> load_state(std::string_view state_path)
{
    std::error_code ec;
    if (!fs::exists(fs::path(state_path), ec))
    {
        AppState fresh; // 初回起動：空の現行 version
        fresh.version = kStateVersion;
        return Result<AppState>::ok(std::move(fresh));
    }
    auto bytes = pika::util::read_all(state_path);
    if (bytes.is_err())
    {
        return Result<AppState>::err(bytes.error());
    }
    return parse_state(bytes.value());
}

Result<void> save_state(std::string_view state_path, const AppState& state)
{
    std::error_code ec;
    fs::create_directories(fs::path(state_path).parent_path(), ec);
    return pika::util::write_atomic(state_path, serialize_state(state));
}

} // namespace pika::core::state
