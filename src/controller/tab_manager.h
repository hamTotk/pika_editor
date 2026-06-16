// controller/tab_manager: タブモデルと状態機械（wx 非依存）。
// design.md 5.1 手順4（消失タブ安全遷移）・5.6（フォルダ切替）・10章 / ui-design 5章（重畳状態の
// 表示優先）/ 要件5.3・7.2 / spec.md sprint2 must。
//
// タブの open/close/activate と、各タブの重畳状態（削除済み・未保存・差分あり）を表示優先
// （削除済み ＞ 未保存 ＞ 差分あり。ui-design 5章）で 1
// つの表示マークへ畳み込む。消失タブ（外部削除で
// パスが消えたタブ）は削除済み表示へ安全遷移し、アクティブタブが消えても破綻しない（design.md 5.1
// 手順4）。wxAuiNotebook（GUI
// スプリント）はこの結果を描画するだけにし、状態遷移の判断はここに集約する。
#pragma once

#include "controller/tree_view_model.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pika::controller
{

// 1 タブの状態（重畳しうるフラグ集合）。表示マークは resolve（display_mark）で導出する。
struct TabState
{
    std::string path;          // 開いているファイルの絶対パス（タブの同一性キー）
    std::string title;         // タブ見出し（表示名。通常はファイル名）
    bool unsaved = false;      // エディタ編集の dirty（● 未保存）
    bool unread = false;       // 外部変更で未読（± 差分あり）
    bool has_baseline = true;  // ベースラインを持つか（false かつ未読＝新規 ◆）
    bool path_missing = false; // 外部削除でパス消失（取り消し線 削除済み）
};

// 単一フォルダワークスペースのタブ集合と、アクティブタブを管理する状態機械。
// すべての遷移は決定論的（同一操作列で同一状態）。GUI 非依存。
class TabManager
{
  public:
    // 既存タブ（同一 path）があればそれをアクティブにし index を返す（重複オープンを開かない）。
    // 無ければ末尾に追加してアクティブにし、その index を返す。
    std::size_t open(const std::string& path, const std::string& title);

    // index のタブを閉じる。アクティブタブを閉じた場合は安全な隣接タブへアクティブを移す
    // （右隣優先、無ければ左隣。design.md 5.1 手順4「消失で破綻しない」と同じ安全遷移）。
    // 範囲外 index は何もしない（クラッシュしない）。
    void close(std::size_t index);

    // index のタブをアクティブにする。範囲外は無視（現アクティブを変えない）。
    void activate(std::size_t index);

    // 外部削除等でパスが消えたタブを「削除済み表示」へ安全遷移させる（バッファは保持。要件7.2）。
    // 該当 path のタブが無ければ何もしない。アクティブタブが消失しても閉じず削除済み表示で残す
    // （ユーザーの未確認内容を失わない。design.md 5.1 手順4）。
    void mark_path_missing(const std::string& path);

    // path のタブの未保存フラグを設定する（エディタ編集/保存完了で UI から呼ぶ）。
    void set_unsaved(const std::string& path, bool unsaved);

    // path のタブの未読フラグを設定する（外部変更検知/確認済みで呼ぶ）。has_baseline は新規 ◆
    // 弁別用。
    void set_unread(const std::string& path, bool unread, bool has_baseline);

    std::size_t count() const noexcept { return tabs_.size(); }
    bool empty() const noexcept { return tabs_.empty(); }

    // アクティブタブの index（タブが無ければ kNoActive）。
    static constexpr std::size_t kNoActive = static_cast<std::size_t>(-1);
    std::size_t active_index() const noexcept { return active_; }

    // index のタブ状態（範囲外は nullptr）。
    const TabState* at(std::size_t index) const;

    // path で検索（無ければ kNoActive）。
    std::size_t index_of(const std::string& path) const;

  private:
    std::vector<TabState> tabs_;
    std::size_t active_ = kNoActive;
};

// タブの重畳状態を表示優先（削除済み ＞ 未保存 ＞ 差分あり）で 1 つの StateMark へ畳み込む
// （ui-design 5章・要件5.3）。tree_view_model の resolve_file_mark と同一の優先順位を用い、
// タブ側の状態（NodeStateInput）へ写して一元化する（記号体系をツリーとタブで分散させない）。
StateMark display_mark(const TabState& tab);

// ---- フォルダ切替の状態機械（design.md 5.6。should criteria） ----

// フォルダ切替の段階（design.md 5.6 手順 0〜3）。未保存確認 → 後始末 → 新フォルダ列挙開始。
enum class FolderSwitchPhase
{
    Idle,            // 切替中でない
    ConfirmUnsaved,  // 1: 未保存タブの確認（保存/破棄/キャンセル）
    TeardownCurrent, // 2: 現ワークスペースの後始末（監視停止・index/state 書き出し・閉じる）
    EnumerateNew,    // 3: 新フォルダ列挙・監視開始・未読判定（5.1 手順4 を実行）
    Cancelled,       // ユーザーがキャンセル → 切替中止（現ワークスペース維持）
};

// 未保存確認に対するユーザーの選択（design.md 5.6 手順1）。
enum class UnsavedChoice
{
    SaveAll, // 保存して継続
    Discard, // 破棄して継続
    Cancel,  // 切替を中止
};

// フォルダ切替の状態機械。has_unsaved（切替開始時に未保存タブがあるか）で初期段階を分岐し、
// ユーザー選択で確定的に遷移する。実 I/O（保存・列挙）は呼び出し側が各段階で行う。
class FolderSwitch
{
  public:
    // 切替を開始する。未保存があれば ConfirmUnsaved、無ければ即 TeardownCurrent へ。
    FolderSwitchPhase begin(bool has_unsaved);

    // ConfirmUnsaved に対するユーザー選択を適用して次段階へ。
    //   SaveAll/Discard → TeardownCurrent、Cancel → Cancelled。
    // ConfirmUnsaved 以外の段階で呼ぶと現段階を変えない（安全側）。
    FolderSwitchPhase resolve_unsaved(UnsavedChoice choice);

    // 後始末完了の通知。TeardownCurrent → EnumerateNew へ。それ以外では現段階を変えない。
    FolderSwitchPhase teardown_done();

    FolderSwitchPhase phase() const noexcept { return phase_; }

  private:
    FolderSwitchPhase phase_ = FolderSwitchPhase::Idle;
};

} // namespace pika::controller
