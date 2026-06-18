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
#include <vector>

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

    // 現在展開中のフォルダノードの相対パス列（state.json 保存用。要件10.1・F1）。
    std::vector<std::string> expanded_rel_paths() const;

    // 相対パス列に該当するフォルダノードを展開する（state.json 復元・ベストエフォート）。
    // 現在ツリーに存在しないパスは無視する（落ちない＝データを失わない・設計原則1）。
    void expand_rel_paths(const std::vector<std::string>& rel_paths);

  private:
    void on_item_activated(wxDataViewEvent& evt);

    wxDataViewCtrl* view_ = nullptr;
    wxObjectDataPtr<FileTreeModel> model_;
    OnFileActivated on_activated_;
};

} // namespace pika::ui
