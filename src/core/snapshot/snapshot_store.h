// core/snapshot: ベースライン・退避の保存/復元と容量管理（snapshot の中核）。
// design.md 7章・9章 / 要件9章。設計原則1「データを失わない」が最優先（退避＝最後の砦）。
//
// データルート配下 snapshots\<wsKey>\{index.json, objects\<hash>} を管理する。本クラスは UI
// 非依存で 実 FS を触る（gtest はテンポラリのデータルートで検証）。容量管理（ファイルごと最新10件
// LRU・ 容量GC500MB・90日GC・未復元退避14日保護・mark-and-sweep）まで本クラスが担う。
//
// 【保存順序の不変条件（呼び出し側の責務）】 add_stash / revert_batch / enforce_capacity は
// in-memory の SnapshotIndex を更新する過程で、参照されなくなった object を**即座に物理削除**する
// （mark-and-sweep）。一方 index.json
// のディスク保存（save）は本クラスでは行わず呼び出し側に委ねる。 したがって呼び出し側は「load →
// 操作 → save」を 1 まとまりとして扱い、操作後はできるだけ早く save
// すること。これを怠ると、操作後〜save 前にクラッシュした場合に古い index.json が既に削除された
// object を指すダングリング参照の窓が残る（restore_* が静かに NotFound を返す）。本窓は将来
// DocumentController で load→操作→save を原子化して塞ぐ前提（report.md 持ち越し #5）。
#pragma once

#include "core/snapshot/object_store.h"
#include "core/snapshot/snapshot_types.h"
#include "util/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pika::core::snapshot
{

// ワークスペースの正規化絶対パスから wsKey（XXH3 hex）を作る。
// 単体ファイル・ワークスペース外ファイルは "file-" 接頭辞で同じ仕組みに載せる（要件9.1）。
std::string workspace_key(std::string_view normalized_path);
std::string file_key(std::string_view normalized_file_path);

// 容量・期間管理の既定値（要件9.3）。テストから上書きできるよう構造体で渡す。
struct CapacityPolicy
{
    std::size_t per_file_stash_limit = 10;           // ファイルごと最新10件 LRU
    std::uint64_t total_byte_limit = 500ull << 20;   // 全体上限500MB
    std::int64_t age_gc_seconds = 90LL * 24 * 3600;  // 90日GC
    std::int64_t protect_seconds = 14LL * 24 * 3600; // 未復元退避14日保護
};

// 退避種別の入力（StashKind と内容）。
struct StashRequest
{
    std::string rel_path;
    StashKind kind = StashKind::Conflict;
    std::string content;  // 退避する内容（原文。機密ファイルでも退避は内容を保持する）
    std::string batch_id; // baseline-replace のときのみ設定（一括取消単位）
};

// 1 ワークスペース分の snapshot を扱う。コンストラクタは FS を触らない（遅延作成）。
class SnapshotStore
{
  public:
    // snapshots_root = データルート配下 "snapshots" の絶対パス。ws_key = workspace_key/file_key
    // の結果。
    SnapshotStore(std::string snapshots_root, std::string ws_key);
    SnapshotStore(std::string snapshots_root, std::string ws_key, CapacityPolicy policy);

    // index を読み込む（破損・未知 version はそのまま伝播。呼び出し側が復元/安全側を選ぶ）。
    pika::util::Result<SnapshotIndex> load();
    // index をアトミックに保存する。
    pika::util::Result<void> save(const SnapshotIndex& index);

    // ベースラインを取得/更新する。除外（機密）ファイル・10MB以上・画像は内容 object を保存せず
    // baselineHash のみ記録する（is_sensitive=true か content_object_allowed=false で制御）。
    // 戻り値は更新後の IndexEntry（unread=false に解除する）。
    pika::util::Result<IndexEntry> set_baseline(SnapshotIndex& index, const std::string& rel_path,
                                                std::string_view content, std::int64_t mtime,
                                                bool sensitive, bool content_object_allowed);

    // ベースライン内容を復元する（baselineObject から展開）。内容を持たない（ハッシュのみ記録）
    // ファイルは ErrorCode::Unsupported（巻き戻し・差分の非活性に対応）。
    pika::util::Result<std::string> restore_baseline(const SnapshotIndex& index,
                                                     const std::string& rel_path) const;

    // 退避を追加する。内容 object を put_stash で格納し、index のエントリ stash に追記する。
    // per-file LRU（最新10件）を適用し、あふれた退避 object は mark-and-sweep で物理削除する。
    // baseline-replace は10件枠とは別バッチとして保持する（要件9.2）。
    pika::util::Result<StashEntry> add_stash(SnapshotIndex& index, const StashRequest& req,
                                             std::int64_t time);

    // 退避内容を復元する（object hash から展開）。restored フラグは呼び出し側が index で更新する。
    pika::util::Result<std::string> restore_stash(const std::string& object_hash) const;

    // baseline-replace バッチを一括取消する（同一 batch_id の退避を全エントリから除去し、
    // 参照されなくなった object を mark-and-sweep で削除する）。取消件数を返す。
    std::size_t revert_batch(SnapshotIndex& index, const std::string& batch_id);

    // 容量管理を実行する（per-file LRU → 容量GC500MB → 90日GC、未復元14日保護を侵さない）。
    // last_opened_epoch はワークスペースを最後に開いた時刻（90日GC の基準）。
    // 削除された object 数を返す。index は破壊的に更新される（呼び出し側が save する）。
    std::size_t enforce_capacity(SnapshotIndex& index, std::int64_t now, std::int64_t last_opened);

    // index に存在する全ベースライン/退避から参照される object を集合化し、どこからも参照されない
    // objects を物理削除する（mark-and-sweep。共有実体の誤削除防止。D5）。削除数を返す。
    std::size_t sweep_unreferenced_objects(const SnapshotIndex& index);

    // ワークスペース/ファイルを閉じた際の手動パージ（index・objects を一括削除。要件9.4）。
    void purge();

    // index.json 破損時に objects のサイドカー走査から復元待ち退避一覧を返す（D1）。
    std::vector<RecoveredStash> recover_pending_stashes() const;

    ObjectStore& objects() noexcept { return objects_; }
    const std::string& ws_dir() const noexcept { return ws_dir_; }
    std::string index_path() const;

  private:
    // ファイルごと最新10件 LRU を 1 エントリに適用する。あふれた非バッチ退避を古い側から落とすが、
    // 未復元かつ生成から protect_seconds 以内の退避は保護対象として残す（要件9.3。保護が LRU より
    // 優先）。add_stash と enforce_capacity が同じ規則を共有するための単一実装（経路乖離の防止）。
    void apply_per_file_lru(IndexEntry& entry, std::int64_t now) const;

    std::string ws_dir_;
    ObjectStore objects_;
    CapacityPolicy policy_;
};

} // namespace pika::core::snapshot
