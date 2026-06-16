// ui/file_tree_panel: 左ペインのファイルツリー（wxDataViewCtrl）。
// design.md 10章（wxDataViewCtrl 第一候補・種別アイコン＋状態マーク共存）/ ui-design 5/6章 /
// 要件4章 / spec.md sprint3 must（種別アイコン＋テキスト＋状態マークを列内描画・逐次追加列挙）。
//
// 表示属性（状態マーク種別・アイコンカテゴリ）の決定は controller::build_tree_view_model（wx
// 非依存・ gtest 済み）が行い、本クラスはその TreeRowVm 木を wxDataViewModel
// に流して描画するだけにする。 状態記号文字・アクセシブルネームは
// controller/tree_view_messages（単一定義）から取得する。
// 逐次追加列挙（フォルダを開くと子バッチが届く）に備え、ノード追加 API を持つ（UI
// スレッドで呼ぶ）。
#pragma once

#include "controller/tree_view_model.h"

#include <wx/dataview.h>
#include <wx/panel.h>

#include <functional>
#include <string>

namespace pika::ui
{

// ファイル（フォルダでない行）がアクティブ化（ダブルクリック/Enter）されたときの通知。
// rel_path はワークスペースルート起点の相対パス。MainFrame がこれをタブで開く。
using OnFileActivated = std::function<void(const std::string& rel_path)>;

class FileTreeModel; // 前方宣言（実装ファイル内の wxDataViewModel 派生）。

class FileTreePanel : public wxPanel
{
  public:
    explicit FileTreePanel(wxWindow* parent);
    ~FileTreePanel() override;

    // TreeViewModel 木で全置換する（フォルダオープン直後・全再列挙時）。
    void set_root(const controller::TreeRowVm& root);

    // ファイルアクティブ化のハンドラを設定する。
    void set_on_file_activated(OnFileActivated cb);

  private:
    void on_item_activated(wxDataViewEvent& evt);

    wxDataViewCtrl* view_ = nullptr;
    wxObjectDataPtr<FileTreeModel> model_;
    OnFileActivated on_activated_;
};

} // namespace pika::ui
