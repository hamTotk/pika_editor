#include "ui/file_tree_panel.h"

#include "controller/tree_view_messages.h"
#include "ui/ui_messages.h"

#include <wx/generic/dataview.h>
#include <wx/sizer.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pika::ui
{

namespace
{

// 状態マーク種別を行頭の記号テキストへ写す（色非依存。記号は単一定義 tree_view_messages から）。
wxString mark_prefix(controller::StateMark mark)
{
    const std::string_view sym = controller::state_mark_symbol(mark);
    if (sym.empty())
    {
        return wxString();
    }
    return wxString::FromUTF8(sym.data(), sym.size()) + " ";
}

} // namespace

// wxDataViewModel: TreeRowVm 木をそのまま読み出す軽量モデル。
// ノード identity は TreeRowVm* ポインタ（モデルが所有する木の安定アドレス）。
class FileTreeModel : public wxDataViewModel
{
  public:
    void set_root(const controller::TreeRowVm& root)
    {
        root_ = std::make_unique<controller::TreeRowVm>(root);
        Cleared();
    }

    unsigned int GetColumnCount() const override { return 1; }
    wxString GetColumnType(unsigned int) const override { return "string"; }

    void GetValue(wxVariant& value, const wxDataViewItem& item, unsigned int /*col*/) const override
    {
        const auto* node = to_node(item);
        if (node == nullptr)
        {
            value = wxString();
            return;
        }
        // 列内に「状態記号＋表示名」を描画する（種別アイコンは将来 wxDataViewIconText で重畳。
        // 本 sprint は記号＋テキストを列内に出し、アクセシブルネームに状態ラベルを含める）。
        value = mark_prefix(node->mark) + wxString::FromUTF8(node->name.c_str(), node->name.size());
    }

    bool SetValue(const wxVariant&, const wxDataViewItem&, unsigned int) override { return false; }

    // 行の視覚属性。削除済み（Deleted）のみ取り消し線を付ける（ui-design
    // 5章「削除済み＝取り消し線・記号なし」）。
    // 差分あり（±）・新規（◆）等は記号＋色で示すため、ここでは取り消し線を付けない（現状維持）。
    bool GetAttr(const wxDataViewItem& item, unsigned int /*col*/,
                 wxDataViewItemAttr& attr) const override
    {
        const auto* node = to_node(item);
        if (node != nullptr && node->mark == controller::StateMark::Deleted)
        {
            attr.SetStrikethrough(true);
            return true;
        }
        return false;
    }

    // 行のアクセシブルネーム（UIA/MSAA。ui-design 13章「色だけに依存しない＝記号＋ラベル」）。
    // 表示テキスト（GetValue 本文）は「記号＋名前」のまま据え置き、スクリーンリーダー向けに
    // 「名前, 状態ラベル, 種別ラベル」を別経路（FileTreeAccessible）へ供給する。
    // 状態・種別の日本語ラベルは controller/tree_view_messages（単一定義）から取得する。
    wxString accessible_name(const wxDataViewItem& item) const
    {
        const auto* node = to_node(item);
        if (node == nullptr)
        {
            return wxString();
        }
        wxString name = wxString::FromUTF8(node->name.c_str(), node->name.size());
        const std::string_view state = controller::state_mark_label(node->mark);
        if (!state.empty())
        {
            name += ", " + wxString::FromUTF8(state.data(), state.size());
        }
        const std::string_view category = controller::icon_category_label(node->icon);
        if (!category.empty())
        {
            name += ", " + wxString::FromUTF8(category.data(), category.size());
        }
        return name;
    }

    wxDataViewItem GetParent(const wxDataViewItem& item) const override
    {
        const auto* node = to_node(item);
        if (node == nullptr || node == root_.get())
        {
            return wxDataViewItem(nullptr);
        }
        const auto* parent = find_parent(root_.get(), node);
        // ルート直下の親はルート自身でなく不可視ルート（nullptr）として扱う。
        if (parent == root_.get())
        {
            return wxDataViewItem(nullptr);
        }
        return wxDataViewItem(const_cast<controller::TreeRowVm*>(parent));
    }

    bool IsContainer(const wxDataViewItem& item) const override
    {
        if (!item.IsOk())
        {
            return true; // 不可視ルートはコンテナ。
        }
        const auto* node = to_node(item);
        return node != nullptr && node->is_dir;
    }

    unsigned int GetChildren(const wxDataViewItem& item,
                             wxDataViewItemArray& children) const override
    {
        const controller::TreeRowVm* node = item.IsOk() ? to_node(item) : root_.get();
        if (node == nullptr)
        {
            return 0;
        }
        for (const auto& child : node->children)
        {
            children.Add(wxDataViewItem(const_cast<controller::TreeRowVm*>(&child)));
        }
        return static_cast<unsigned int>(node->children.size());
    }

    bool is_dir(const wxDataViewItem& item) const
    {
        const auto* node = to_node(item);
        return node != nullptr && node->is_dir;
    }

    std::string rel_path(const wxDataViewItem& item) const
    {
        const auto* node = to_node(item);
        return node != nullptr ? node->rel_path : std::string();
    }

    // 全フォルダノードを (相対パス, item) で列挙する（展開状態の収集・復元に使う。state.json）。
    std::vector<std::pair<std::string, wxDataViewItem>> dir_items() const
    {
        std::vector<std::pair<std::string, wxDataViewItem>> out;
        if (root_)
        {
            collect_dirs(root_.get(), out);
        }
        return out;
    }

  private:
    static void collect_dirs(const controller::TreeRowVm* node,
                             std::vector<std::pair<std::string, wxDataViewItem>>& out)
    {
        for (const auto& child : node->children)
        {
            if (child.is_dir)
            {
                out.emplace_back(child.rel_path,
                                 wxDataViewItem(const_cast<controller::TreeRowVm*>(&child)));
                collect_dirs(&child, out);
            }
        }
    }

    static const controller::TreeRowVm* to_node(const wxDataViewItem& item)
    {
        return static_cast<const controller::TreeRowVm*>(item.GetID());
    }

    // node の親を木から線形探索する（ツリー規模は中小。frame 内で十分）。
    static const controller::TreeRowVm* find_parent(const controller::TreeRowVm* cur,
                                                    const controller::TreeRowVm* target)
    {
        for (const auto& child : cur->children)
        {
            if (&child == target)
            {
                return cur;
            }
            const auto* found = find_parent(&child, target);
            if (found != nullptr)
            {
                return found;
            }
        }
        return nullptr;
    }

    std::unique_ptr<controller::TreeRowVm> root_;
};

// wxDataViewCtrl の薄い派生。protected な GetItemByRow（行番号→item の解決）を public
// 公開するためだけに存在する（using による親 protected メンバの公開は C++ 標準で合法）。
// AppendTextColumn/AssociateModel 等は親の API をそのまま使う。
class FileTreeDataView : public wxDataViewCtrl
{
  public:
    using wxDataViewCtrl::GetItemByRow;
    using wxDataViewCtrl::wxDataViewCtrl;
};

#if wxUSE_ACCESSIBILITY
// ファイルツリーのアクセシブル実装（UIA/MSAA。ui-design 13章）。
// 既定の wxDataViewCtrlAccessible は各行のアクセシブルネームを「最初の文字列列の表示テキスト」
// （＝記号＋名前）から組み立てる（generic/datavgen.cpp の GetName）。本クラスはその GetName を
// 差し替え、視覚は据え置いたまま「名前, 状態ラベル, 種別ラベル」をスクリーンリーダーへ供給する。
// ナビゲーション・ロール等の挙動は基底をそのまま使う（最小差分）。
class FileTreeAccessible : public wxDataViewCtrlAccessible
{
  public:
    FileTreeAccessible(FileTreeDataView* ctrl, const FileTreeModel* model)
        : wxDataViewCtrlAccessible(ctrl), ctrl_(ctrl), model_(model)
    {
    }

    wxAccStatus GetName(int childId, wxString* name) override
    {
        const wxString accessible = item_accessible_name(childId);
        if (!accessible.empty())
        {
            *name = accessible;
            return wxACC_OK;
        }
        // 行を解決できない場合（自身・行外）は既定の組み立てに委ねる。
        return wxDataViewCtrlAccessible::GetName(childId, name);
    }

  private:
    // childId（1 始まりの行番号。wxACC_SELF=0 はコントロール自身）→ 行 → モデルノードへ写し、
    // 状態ラベル・種別ラベルを含むアクセシブルネームを得る。解決できなければ空。
    wxString item_accessible_name(int childId) const
    {
        if (childId == wxACC_SELF || ctrl_ == nullptr || model_ == nullptr)
        {
            return wxString();
        }
        // 保持した型付きポインタ経由で行→item を解決する（const メソッドから非 const の
        // GetWindow を呼ばずに済む。GetItemByRow は FileTreeDataView で public 公開済み）。
        const wxDataViewItem item = ctrl_->GetItemByRow(static_cast<unsigned int>(childId - 1));
        if (!item.IsOk())
        {
            return wxString();
        }
        return model_->accessible_name(item);
    }

    FileTreeDataView* ctrl_ = nullptr;
    const FileTreeModel* model_ = nullptr;
};
#endif // wxUSE_ACCESSIBILITY

FileTreePanel::FileTreePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY), model_(new FileTreeModel())
{
    // protected な GetItemByRow を public 公開した薄い派生（FileTreeDataView）で生成する。
    // 描画・列・モデル関連付けは親 wxDataViewCtrl の API をそのまま使う。
    auto* view = new FileTreeDataView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                      wxDV_SINGLE | wxDV_ROW_LINES);
    view_ = view;
    view_->AssociateModel(model_.get());
    // 1 列に「状態記号＋アイコン＋テキスト」を共存させる（design 10章・ui-design 6章）。
    // 見出しは生文字列を書かず単一メッセージ定義経由で UTF-8→wx 変換する（design 10章 K9）。
    const std::string header = message(MsgId::TreePaneTitle);
    view_->AppendTextColumn(wxString::FromUTF8(header.c_str(), header.size()), 0,
                            wxDATAVIEW_CELL_INERT, -1, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);

#if wxUSE_ACCESSIBILITY
    // 行ごとのアクセシブルネームに状態・種別ラベルを供給する（視覚は据え置き）。
    // SetAccessible の所有権は wxWindow 側が握り破棄する。
    view_->SetAccessible(new FileTreeAccessible(view, model_.get()));
#endif

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(view_, 1, wxEXPAND);
    SetSizer(sizer);

    view_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, &FileTreePanel::on_item_activated, this);
}

FileTreePanel::~FileTreePanel() = default;

void FileTreePanel::set_root(const controller::TreeRowVm& root)
{
    model_->set_root(root);
    // ルート直下を最初から見せる（逐次列挙では set_root を全置換として呼ぶ）。
}

void FileTreePanel::set_on_file_activated(OnFileActivated cb)
{
    on_activated_ = std::move(cb);
}

std::vector<std::string> FileTreePanel::expanded_rel_paths() const
{
    std::vector<std::string> out;
    for (const auto& [rel, item] : model_->dir_items())
    {
        if (view_->IsExpanded(item))
        {
            out.push_back(rel);
        }
    }
    return out;
}

void FileTreePanel::expand_rel_paths(const std::vector<std::string>& rel_paths)
{
    // 現ツリーに存在する相対パスのみ展開する（消えたパスは無視＝落ちない。設計原則1）。
    // 親→子の順で当てたいので dir_items の列挙順（深さ優先・親が先）に沿って照合する。
    for (const auto& [rel, item] : model_->dir_items())
    {
        if (std::find(rel_paths.begin(), rel_paths.end(), rel) != rel_paths.end())
        {
            view_->Expand(item);
        }
    }
}

void FileTreePanel::on_item_activated(wxDataViewEvent& evt)
{
    const wxDataViewItem item = evt.GetItem();
    if (!item.IsOk())
    {
        return;
    }
    if (model_->is_dir(item))
    {
        // フォルダは展開トグル（wxDataViewCtrl の既定動作に委ねる）。
        if (view_->IsExpanded(item))
        {
            view_->Collapse(item);
        }
        else
        {
            view_->Expand(item);
        }
        return;
    }
    if (on_activated_)
    {
        on_activated_(model_->rel_path(item));
    }
}

} // namespace pika::ui
