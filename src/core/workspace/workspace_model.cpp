#include "core/workspace/workspace_model.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace pika::core::workspace
{

namespace
{

char lower_ascii(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

} // namespace

bool natural_less(std::string_view a, std::string_view b)
{
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size())
    {
        const char ca = a[i];
        const char cb = b[j];
        if (is_digit(ca) && is_digit(cb))
        {
            // 連続する数字列を数値として比較する。先頭ゼロを読み飛ばし、まず有効桁数で大小を決め、
            // 桁数が同じなら左から比較する（file2 < file10。要件4.1）。
            std::size_t sa = i;
            std::size_t sb = j;
            while (sa < a.size() && a[sa] == '0')
            {
                ++sa;
            }
            while (sb < b.size() && b[sb] == '0')
            {
                ++sb;
            }
            std::size_t da = sa;
            std::size_t db = sb;
            while (da < a.size() && is_digit(a[da]))
            {
                ++da;
            }
            while (db < b.size() && is_digit(b[db]))
            {
                ++db;
            }
            const std::size_t len_a = da - sa;
            const std::size_t len_b = db - sb;
            if (len_a != len_b)
            {
                return len_a < len_b;
            }
            // 桁数が同じなら 1 桁ずつ比較する。
            for (std::size_t k = 0; k < len_a; ++k)
            {
                if (a[sa + k] != b[sb + k])
                {
                    return a[sa + k] < b[sb + k];
                }
            }
            // 数値として等しい。元の桁数（先頭ゼロの数）が短い方を前に置いて安定させる。
            if ((da - i) != (db - j))
            {
                return (da - i) < (db - j);
            }
            i = da;
            j = db;
        }
        else
        {
            const char la = lower_ascii(ca);
            const char lb = lower_ascii(cb);
            if (la != lb)
            {
                return la < lb;
            }
            // 大小無視で同じなら、元の文字で安定化（'A' < 'a'）。
            if (ca != cb)
            {
                return ca < cb;
            }
            ++i;
            ++j;
        }
    }
    // 共通接頭辞が一致した場合、短い方を前に置く。
    return (a.size() - i) < (b.size() - j);
}

bool is_excluded(std::string_view rel_path, const std::vector<std::string>& exclude)
{
    if (exclude.empty())
    {
        return false;
    }
    // rel_path を '/' でセグメント分割し、いずれかのセグメント名が除外要素と一致するか見る。
    std::size_t start = 0;
    while (start <= rel_path.size())
    {
        std::size_t slash = rel_path.find('/', start);
        std::string_view seg = rel_path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!seg.empty())
        {
            for (const auto& ex : exclude)
            {
                if (seg == ex)
                {
                    return true;
                }
            }
        }
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return false;
}

namespace
{

// rel_path の親ディレクトリ（'/' 区切り）を辿りつつ、ツリーへ深さ優先で挿入する。
// 途中ディレクトリのノードが無ければ生成する。葉ノードは is_dir で種別を確定する。
TreeNode* ensure_dir_node(TreeNode& root, std::string_view dir_rel)
{
    if (dir_rel.empty())
    {
        return &root;
    }
    TreeNode* cur = &root;
    std::size_t start = 0;
    std::string acc;
    while (start <= dir_rel.size())
    {
        std::size_t slash = dir_rel.find('/', start);
        std::string_view seg = dir_rel.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!acc.empty())
        {
            acc += '/';
        }
        acc.append(seg.data(), seg.size());

        TreeNode* found = nullptr;
        for (auto& child : cur->children)
        {
            if (child.is_dir && child.name == seg)
            {
                found = &child;
                break;
            }
        }
        if (found == nullptr)
        {
            TreeNode node;
            node.name = std::string(seg);
            node.rel_path = acc;
            node.is_dir = true;
            cur->children.push_back(std::move(node));
            found = &cur->children.back();
        }
        cur = found;
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return cur;
}

// フォルダ先行＋自然順で children を再帰的に整列する。
void sort_tree(TreeNode& node)
{
    std::sort(node.children.begin(), node.children.end(), [](const TreeNode& x, const TreeNode& y) {
        if (x.is_dir != y.is_dir)
        {
            return x.is_dir; // フォルダ先行
        }
        return natural_less(x.name, y.name);
    });
    for (auto& child : node.children)
    {
        sort_tree(child);
    }
}

std::pair<std::string_view, std::string_view> split_parent_name(std::string_view rel_path)
{
    std::size_t pos = rel_path.find_last_of('/');
    if (pos == std::string_view::npos)
    {
        return {std::string_view{}, rel_path};
    }
    return {rel_path.substr(0, pos), rel_path.substr(pos + 1)};
}

} // namespace

TreeNode build_tree(const std::vector<Entry>& entries, const std::vector<std::string>& exclude)
{
    TreeNode root;
    root.is_dir = true;
    for (const auto& e : entries)
    {
        if (e.rel_path.empty() || is_excluded(e.rel_path, exclude))
        {
            continue; // 除外配下は監視対象外として木に含めない（要件4.1）
        }
        auto [parent, name] = split_parent_name(e.rel_path);
        TreeNode* dir = ensure_dir_node(root, parent);

        // 既存ノード（中間ディレクトリとして先に生成済み等）があれば種別を上書きしない衝突を避ける。
        TreeNode* existing = nullptr;
        for (auto& child : dir->children)
        {
            if (child.name == name)
            {
                existing = &child;
                break;
            }
        }
        if (existing != nullptr)
        {
            // ディレクトリエントリが明示された場合は is_dir を確定させる。
            if (e.is_dir)
            {
                existing->is_dir = true;
            }
            continue;
        }
        TreeNode node;
        node.name = std::string(name);
        node.rel_path = e.rel_path;
        node.is_dir = e.is_dir;
        dir->children.push_back(std::move(node));
    }
    sort_tree(root);
    return root;
}

void UnreadSet::mark(const std::string& rel_path)
{
    if (!rel_path.empty())
    {
        unread_.insert(rel_path);
    }
}

void UnreadSet::clear(const std::string& rel_path)
{
    unread_.erase(rel_path);
}

bool UnreadSet::is_unread(std::string_view rel_path) const
{
    return unread_.find(std::string(rel_path)) != unread_.end();
}

bool UnreadSet::has_unread_descendant(std::string_view rel_path) const
{
    if (rel_path.empty())
    {
        return !unread_.empty(); // ルートは全未読が子孫
    }
    // prefix = rel_path + "/" で始まる未読が 1 件でもあれば真。set は整列済みなので lower_bound
    // から prefix が続く範囲だけ見れば足りる。
    std::string prefix(rel_path);
    prefix += '/';
    auto it = unread_.lower_bound(prefix);
    if (it == unread_.end())
    {
        return false;
    }
    return it->size() >= prefix.size() && it->compare(0, prefix.size(), prefix) == 0;
}

CarryResult apply_renames(const std::map<std::string, CarryState>& states,
                          const std::vector<RenameOp>& ops)
{
    CarryResult result;
    // 作業用に状態を可変コピーする（rename を順に適用する）。
    std::map<std::string, CarryState> work = states;
    result.outcomes.reserve(ops.size());

    // 往復（A→B→A）検出: ある op `{x→y}` の逆向き `{y→x}` がより前にあれば「短時間の往復で
    // 対応付け確定不能」と見なす（要件4.2）。直前の逆向きペアだけを往復とし、一時退避を介した
    // 正規スワップ（A→tmp, B→A, tmp→B のような逆向きペアを含まない置換列）は通常の引き継ぎとして
    // 処理する（過剰に再判定へ倒さない）。
    std::set<std::pair<std::string, std::string>> seen_pairs;

    for (const auto& op : ops)
    {
        // 旧名単独（new 空）＝削除。状態は孤立保全（90日GC に委ねる。要件4.2/7.2）。
        if (op.new_path.empty())
        {
            if (work.find(op.old_path) != work.end())
            {
                result.orphaned.push_back(op.old_path);
            }
            result.outcomes.push_back(CarryOutcome::Removed);
            continue;
        }
        // 新名単独（old 空）＝新規。ベースラインなしで開始する（要件4.2/8章）。
        if (op.old_path.empty())
        {
            CarryState fresh;
            fresh.unread = true; // 新規作成は未読
            work[op.new_path] = fresh;
            result.outcomes.push_back(CarryOutcome::Created);
            continue;
        }

        // 逆向きペア `{new_path→old_path}` が既出なら短時間の往復（A→B→A）。確定不能のため、
        // 最終ディスク内容で再判定する指示を返す（要件4.2）。
        const bool roundtrip = seen_pairs.count({op.new_path, op.old_path}) > 0;
        seen_pairs.insert({op.old_path, op.new_path});
        if (roundtrip)
        {
            result.reevaluate.push_back(op.new_path);
            result.outcomes.push_back(CarryOutcome::Reevaluated);
            continue;
        }

        auto src = work.find(op.old_path);
        const bool dst_exists = work.find(op.new_path) != work.end();
        if (src == work.end())
        {
            // 旧状態が無い（既に移動済み等）。new は新規相当として扱う。
            CarryState fresh;
            fresh.unread = true;
            work[op.new_path] = fresh;
            result.outcomes.push_back(CarryOutcome::Created);
            continue;
        }

        CarryState moved = src->second;
        work.erase(src);
        // リネーム先に既存エントリがある場合は移動元で上書きする（要件4.2）。
        work[op.new_path] = std::move(moved);
        result.outcomes.push_back(dst_exists ? CarryOutcome::OverwroteDst : CarryOutcome::Moved);
    }

    result.states = std::move(work);
    return result;
}

} // namespace pika::core::workspace
