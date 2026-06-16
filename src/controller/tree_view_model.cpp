#include "controller/tree_view_model.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace pika::controller
{

namespace
{

// 拡張子（'.' を除いた小文字 ASCII）→ カテゴリの対応表（ui-design 6章）。
// 線形数十件で十分（ツリー行数は多くても拡張子種別は少なく、写像は決定論的）。
struct ExtCategory
{
    std::string_view ext;
    IconCategory category;
};

constexpr std::array<ExtCategory, 26> kExtTable = {{
    // コード/マークアップ
    {"ts", IconCategory::Code},
    {"js", IconCategory::Code},
    {"html", IconCategory::Code},
    {"htm", IconCategory::Code},
    {"xml", IconCategory::Code},
    // データ
    {"json", IconCategory::Data},
    // 設定
    {"yaml", IconCategory::Config},
    {"yml", IconCategory::Config},
    {"toml", IconCategory::Config},
    {"ini", IconCategory::Config},
    // スクリプト
    {"sh", IconCategory::Script},
    {"ps1", IconCategory::Script},
    {"bat", IconCategory::Script},
    // 画像
    {"png", IconCategory::Image},
    {"jpg", IconCategory::Image},
    {"jpeg", IconCategory::Image},
    {"gif", IconCategory::Image},
    {"webp", IconCategory::Image},
    {"bmp", IconCategory::Image},
    {"ico", IconCategory::Image},
    {"svg", IconCategory::Image},
    // テキスト/文書
    {"md", IconCategory::Text},
    {"markdown", IconCategory::Text},
    {"txt", IconCategory::Text},
    {"csv", IconCategory::Text},
    {"log", IconCategory::Text},
}};

char lower_ascii(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

} // namespace

StateMark resolve_file_mark(const NodeStateInput& s)
{
    // 重畳時の表示優先：削除済み ＞ 未保存 ＞ 差分あり（ui-design 5章・要件5.3）。
    if (s.deleted)
    {
        return StateMark::Deleted;
    }
    if (s.unsaved)
    {
        return StateMark::Unsaved;
    }
    if (s.unread)
    {
        // 差分あり。ベースラインが無い未読は新規（◆）として ± と弁別する（ui-design 5章）。
        return s.has_baseline ? StateMark::Diff : StateMark::New;
    }
    return StateMark::None;
}

IconCategory classify_icon(std::string_view name_or_ext)
{
    // 最後の '.' 以降を拡張子と解釈する。'.' が無ければ全体を拡張子候補にする
    // （呼び出し側が拡張子だけを渡す場合に対応）。先頭ドットのみ（".env" 等）は拡張子なし扱い。
    std::size_t dot = name_or_ext.find_last_of('.');
    std::string_view ext = name_or_ext;
    if (dot != std::string_view::npos)
    {
        if (dot == 0)
        {
            // 先頭ドットのみ（隠しファイル ".gitignore" 等）は拡張子を持たないと見なす。
            return IconCategory::Unknown;
        }
        ext = name_or_ext.substr(dot + 1);
    }
    if (ext.empty())
    {
        return IconCategory::Unknown;
    }

    // 小文字 ASCII へ正規化して照合する（拡張子は大小無視。要件4.1/5.1 と整合）。
    std::string key;
    key.reserve(ext.size());
    for (char c : ext)
    {
        key.push_back(lower_ascii(c));
    }

    for (const auto& row : kExtTable)
    {
        if (row.ext == key)
        {
            return row.category;
        }
    }
    return IconCategory::Unknown;
}

namespace
{

// 1 ノードを再帰的に ViewModel 化する。new_files は新規（◆）判定用の未読パス集合（呼び出し側で
// ベースライン有無を確定済み）。ファイルは未読集合＋新規集合から状態を、フォルダは子孫伝播から
// 状態を決める。
TreeRowVm convert_node(const core::workspace::TreeNode& node,
                       const core::workspace::UnreadSet& unread,
                       const std::vector<std::string>& new_files)
{
    TreeRowVm vm;
    vm.name = node.name;
    vm.rel_path = node.rel_path;
    vm.is_dir = node.is_dir;

    if (node.is_dir)
    {
        vm.icon = IconCategory::Folder;
        // フォルダ自身に差分（±実心）は付かない。子孫に未読ファイルがあれば伝播 ±（淡）。
        // ルートノード（rel_path 空）も has_unread_descendant("") で全未読を見られる。
        vm.mark =
            unread.has_unread_descendant(vm.rel_path) ? StateMark::DiffPropagated : StateMark::None;
        vm.children.reserve(node.children.size());
        for (const auto& child : node.children)
        {
            vm.children.push_back(convert_node(child, unread, new_files));
        }
    }
    else
    {
        vm.icon = classify_icon(node.name);
        NodeStateInput state;
        state.unread = unread.is_unread(node.rel_path);
        // new_files に含まれるならベースラインなし（新規 ◆）。それ以外の未読は ±（Diff）。
        const bool is_new =
            std::find(new_files.begin(), new_files.end(), node.rel_path) != new_files.end();
        state.has_baseline = !is_new;
        vm.mark = resolve_file_mark(state);
    }
    return vm;
}

} // namespace

TreeRowVm build_tree_view_model(const core::workspace::TreeNode& root,
                                const core::workspace::UnreadSet& unread,
                                const std::vector<std::string>& new_files)
{
    return convert_node(root, unread, new_files);
}

} // namespace pika::controller
