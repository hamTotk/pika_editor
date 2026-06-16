#include "ui/file_tree_panel.h"

#include "controller/tree_view_messages.h"

#include <wx/sizer.h>

#include <memory>
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

  private:
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

FileTreePanel::FileTreePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY), model_(new FileTreeModel())
{
    view_ = new wxDataViewCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxDV_SINGLE | wxDV_ROW_LINES);
    view_->AssociateModel(model_.get());
    // 1 列に「状態記号＋アイコン＋テキスト」を共存させる（design 10章・ui-design 6章）。
    view_->AppendTextColumn("ファイル", 0, wxDATAVIEW_CELL_INERT, -1, wxALIGN_LEFT,
                            wxDATAVIEW_COL_RESIZABLE);

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
