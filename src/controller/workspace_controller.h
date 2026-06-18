// controller/workspace_controller: 中心体験②『外部変更を反映』のアプリケーション層（wx 非依存）。
// design.md 5.2（外部変更の反映・自己保存抑制・rename 追従）/ 要件4.2・7.2 / spec.md sprint4 must。
//
// プラットフォーム層（ReadDirectoryChangesW を回す監視スレッド）が core/watcher へ供給した
// 生イベントを、WatcherCore が合成・正規化・自己保存抑制した確定 FsEvent 列として poll() で得る。
// 本クラスはその確定列を「未読集合（UnreadSet）」「ファイルごとの引き継ぎ状態（ベースライン有無・
// 退避ID）」「消失タブ安全遷移のための削除通知」へ写し、ツリー ViewModel 再構築の素材にする。
//
// wx・Win32・実 FS を一切含まない（時刻・FsEvent・FileStat を注入して gtest で決定論検証する）。
// 実 FS 読み取り（確定読み・再列挙）は WatcherCore / resync が担い、本クラスは状態反映に徹する。
#pragma once

#include "controller/tree_view_model.h"
#include "core/watcher/fs_event.h"
#include "core/watcher/resync.h"
#include "core/workspace/workspace_model.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pika::controller
{

// 1 件の FsEvent を反映した結果として UI 側へ伝える分類（通知バー・タブ・ツリー更新の振り分け用）。
// design.md 5.2「クリーン時自動リロード・削除は安全遷移・rename は追従」。
enum class FsChangeEffect
{
    UnreadMarked,  // Created/Modified → 当該ファイルを未読化（ツリー ±/◆・タブ ±）
    PathRemoved,   // Removed → 消失タブ安全遷移（削除済み表示）。状態は孤立保全で残す
    RenamedCarried // Renamed → 旧パスの未読・ベースライン・退避を新パスへ引き継いだ
};

// 1 件の FsEvent を反映した結果（呼び出し側が UI へ写す素材）。
struct FsChange
{
    FsChangeEffect effect = FsChangeEffect::UnreadMarked;
    std::string path;     // 影響対象パス（Renamed のときは新パス）
    std::string old_path; // Renamed のときのみ意味を持つ（旧パス）
    bool is_new = false;  // UnreadMarked かつベースライン無し＝新規（◆）。それ以外は false
    // Renamed のときのみ。apply_renames が「対応付け不能で最終ディスク内容で再判定せよ」と
    // 判定したパス（往復 A→B→A 等。要件4.2）。本 rename イベントの引き継ぎで生じた分だけを
    // 載せる（controller 側でベースラインを暫定無効化し再差分対象化する）。
    std::vector<std::string> reevaluate;
    // Renamed のときのみ。引き継ぎ失敗で旧キーが孤立保全されたパス
    // （90日GC に委ねる。要件4.2/7.2）。握り潰さず UI/ログへ流すための素材。
    std::vector<std::string> orphaned;
};

// WorkspaceController: 外部変更の反映ロジック（wx 非依存）。
// 内部に UnreadSet と「ファイル rel_path → 引き継ぎ状態（CarryState）」を持ち、
// poll() が返した FsEvent 列を apply_events で取り込んで両者を更新する。
class WorkspaceController
{
  public:
    // ワークスペースルート（'/' 区切り絶対パス）。ツリー ViewModel 構築と相対化に使う。
    explicit WorkspaceController(std::string root = {});

    // 起動時/再同期後のベースライン（rel_path → 確定済みメタ）を取り込む。
    // ここに載るファイルは「ベースラインあり」＝以後の未読は ±（Diff）、載らない未読は新規（◆）。
    void set_baseline(const core::watcher::BaselineMap& baseline);

    // 現在のベースライン（resync の突き合わせ基準）。set_baseline で取り込んだメタを保持する。
    // resync(root, baseline) は無変化ファイル（mtime+size 一致）を未読化しないために使う。
    const core::watcher::BaselineMap& baseline() const noexcept { return baseline_; }

    // FsEvent 列（WatcherCore::poll または resync の結果）を取り込み、未読集合と引き継ぎ状態を
    // 更新する。戻り値は各イベントの反映結果（順序は入力順）。同一入力で同一出力（純粋）。
    std::vector<FsChange> apply_events(const std::vector<core::watcher::FsEvent>& events);

    // 「確認済みにする」で 1 ファイルの未読を解除する（要件4.2/8章。確認済みフローは sprint6）。
    void mark_confirmed(const std::string& rel_path);

    // 現在の未読集合（ツリー ViewModel・ステータスバー未読件数に使う）。
    const core::workspace::UnreadSet& unread() const noexcept { return unread_; }

    // 確認済みフロー（DocumentController）が未読集合を直接更新するための可変参照（design 5.4）。
    // 確認成功で未読を除去・一括取消で未読へ戻す等、退避結合の結果を 1 つの未読集合へ反映するため、
    // WorkspaceController と DocumentController が同じ集合を共有する（二重管理を避ける）。
    core::workspace::UnreadSet& unread_mut() noexcept { return unread_; }

    // 新規（◆）として表示すべき未読ファイル（ベースラインを持たない未読）の rel_path 集合。
    // build_tree_view_model の new_files 引数へそのまま渡す。
    std::vector<std::string> new_files() const;

    // rename 引き継ぎで「対応付け不能＝最終ディスク内容で再判定せよ」と判定された累積パス集合
    // （要件4.2）。controller は当該エントリのベースラインを暫定無効化（再差分対象化）済みだが、
    // UI/ログへ「再判定が必要」と通知する素材としても観測できるよう累積保持する。
    const std::vector<std::string>& reevaluate_pending() const noexcept { return reevaluate_; }

    // rename 引き継ぎで旧キーが孤立保全された累積パス集合（90日GC に委ねる。要件4.2/7.2）。
    // 握り潰さず UI/ログ（消失タブ安全遷移・退避保全の診断）へ流す素材として観測できる。
    const std::vector<std::string>& orphaned() const noexcept { return orphaned_; }

    // 外部削除されたファイルの相対パス集合（要件11.5・ui-design 5章・F-017）。ディスク再列挙では
    // 削除ファイルが落ちるため、ツリー ViewModel へ取り消し線リーフを補うマージ素材として保持する。
    // Removed で追加、Created/Modified/rename で当該パスが再びディスクに現れたら除去する。
    const std::vector<std::string>& deleted_paths() const noexcept { return deleted_; }

    // ツリーノード（build_tree の結果）から、現在の未読/新規状態を載せた ViewModel を構築する。
    // 純ロジックの集約点（tree_view_model.build_tree_view_model を現状態で呼ぶだけ）。
    TreeRowVm build_view_model(const core::workspace::TreeNode& tree) const;

    // ファイルごとの引き継ぎ状態（rename 追従・退避 ID 紐付けのスナップショット）。テスト/診断用。
    const std::map<std::string, core::workspace::CarryState>& states() const noexcept
    {
        return states_;
    }

    const std::string& root() const noexcept { return root_; }

  private:
    // states_ の当該エントリ（無ければ作る）を未読・ベースライン有無で更新する。
    void touch_unread(const std::string& rel_path, bool has_baseline);

    // events[begin, end) の連続 Renamed run を 1 回の apply_renames でまとめて適用し、
    // reevaluate/orphaned を消費して各イベントの FsChange を changes へ積む。
    void apply_rename_run(const std::vector<core::watcher::FsEvent>& events, std::size_t begin,
                          std::size_t end, std::vector<FsChange>& changes);

    std::string root_;
    core::watcher::BaselineMap baseline_; // resync 突き合わせ基準（set_baseline が取り込む）
    core::workspace::UnreadSet unread_;
    // rel_path → 引き継ぎ状態（未読・ベースライン有無・退避 ID）。apply_renames が付け替える。
    std::map<std::string, core::workspace::CarryState> states_;
    // apply_renames の reevaluate/orphaned を握り潰さず累積する（UI/ログへ流す素材）。
    std::vector<std::string> reevaluate_;
    std::vector<std::string> orphaned_;
    // 外部削除パス集合（取り消し線マージ素材。F-017）。重複を避けるため挿入時に存在確認する。
    std::vector<std::string> deleted_;
};

} // namespace pika::controller
