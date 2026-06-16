// controller/document_controller: 中心体験④『確認済みにする』と保存・衝突退避の結線（wx 非依存）。
// design.md 5.3（保存・衝突）・5.4（既読化＝確認済み/すべて確認済み/巻き戻し）/ 要件7.3・8.3 /
// spec.md sprint6 must。
//
// core/document::ReviewFlow（退避フロー結合）と core/snapshot::SnapshotStore（退避・ベースライン・
// 容量管理）を 1 ワークスペース分だけ束ね、UI からの「確認済みにする・すべて確認済み・巻き戻し・
// 保存」をオーケストレーションする。ReviewFlow/SnapshotStore は実 FS（退避 object・index.json）を
// 触るが、本クラス自身は wx・WebView2・Win32 を含まない（gtest はテンポラリのデータルートで検証）。
//
// 設計原則1「データを失わない」を UI 側でも死守する核心：退避結合（baseline-replace・incoming）の
// Result<T> を握り潰さず、失敗時はベースラインを更新せず（未読維持）エラー結果として返す。
// 呼び出し側（GUI）はこの結果を通知バー/診断ログへ変換する（前回 report 持ち越し #5 の UI
// 側ガード）。
//
// 【index の所有】SnapshotStore/ReviewFlow と同一規約で、呼び出し側が load→操作→save する index を
// 参照で受け取り破壊的に更新する。本クラスは未読集合（UnreadSet）への反映までを担い、index.json の
// 永続化（store.save）は呼び出し側が操作直後に行う（snapshot_store.h の保存順序不変条件）。
#pragma once

#include "core/document/review_flow.h"
#include "core/snapshot/snapshot_store.h"
#include "core/snapshot/snapshot_types.h"
#include "core/workspace/workspace_model.h"
#include "util/encoding.h"
#include "util/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pika::controller
{

// 「確認済みにする」1 件の結果（UI 反映の素材）。unread_cleared は未読集合から外したかを示す。
struct ConfirmOutcome
{
    std::string rel_path;
    bool unread_cleared = false;   // 未読集合から除去しツリー/タブのマークを解除したか
    bool baseline_updated = false; // ベースラインを更新したか（失敗時 false＝未読維持）
};

// 「すべて確認済み」の UI 反映結果（core の AllConfirmedResult を controller 層で薄く包む）。
// confirmed は未読解除済み rel_path、skipped は並行変化/退避失敗で未読のまま残した rel_path。
struct ConfirmAllOutcome
{
    std::string batch_id;               // 一括取消の単位
    std::vector<std::string> confirmed; // ベースライン更新＋未読解除した rel_path
    std::vector<std::string> skipped;   // 退避失敗/並行変化でスキップ＝未読維持
};

// 保存前チェックの結果（design.md 5.3）。GUI はこの判定を見て上書き保存に進むか中断する。
// 退避（incoming）が必要かつ取れた場合は stash_hash に退避 object 名が入る。
enum class SaveDecision
{
    Proceed,            // 上書き保存してよい（衝突なし、または衝突を承知で incoming 退避済み）
    BlockedEncoding,    // 現エンコーディングで表現不能な文字を含む＝保存中断（要件5.2・G2）
    BlockedUnstashable, // 衝突したが退避を取れない（10MB以上・画像・機密）＝既定ブロック（要件7.3）
    BlockedStashFailed, // 衝突して退避を試みたが退避 I/O に失敗＝上書きをブロック（データ保全）
};

// 保存前チェックの結果（SaveDecision ＋ 衝突有無 ＋ incoming 退避 object）。
struct SavePlan
{
    SaveDecision decision = SaveDecision::Proceed;
    bool conflict = false;       // 現ディスク内容が最後の読み込み時点と異なる（衝突）
    std::string stash_hash;      // 衝突時に退避した外部内容の object 名（incoming）。退避なら非空
    pika::util::ErrorInfo error; // Blocked* のときの理由（診断ログ/通知バー用。内容は書かない）

    bool ok() const noexcept { return decision == SaveDecision::Proceed; }
};

// 保存前チェックの入力（GUI が DocState/エディタ/ディスクから集めて渡す）。
struct SaveContext
{
    std::string rel_path;
    std::string buffer_content;   // エディタの現バッファ（保存する内容。UTF-8）
    std::string disk_content;     // 現ディスク上の実内容（再読込・ハッシュ再計算済み）
    std::string last_loaded_hash; // 最後に読み込んだ時点の内容ハッシュ（LF 正規化 hex）
    pika::util::Encoding encoding =
        pika::util::Encoding::Utf8; // 保存に使うエンコーディング（表現可能性チェック対象）
    pika::core::document::FileContentClass
        cls;               // 退避可否（10MB以上・画像・機密で can_stash=false）
    std::int64_t time = 0; // 退避時刻（Unix epoch 秒）
};

// DocumentController: 確認済みフロー・保存衝突フローのオーケストレータ。
// 1 ワークスペース分の SnapshotStore を束ね、その上に ReviewFlow を載せる。未読集合への反映を担い、
// 退避結合の Result を握り潰さない。
class DocumentController
{
  public:
    explicit DocumentController(pika::core::snapshot::SnapshotStore& store)
        : store_(store), flow_(store)
    {
    }

    // 「確認済みにする」（design.md 5.4 / 要件8.3）：ディスク内容でベースラインを更新し、成功したら
    // 未読集合から除去する。ベースライン更新が失敗（退避不能・I/O 障害）したら未読を維持し、その
    // Result を握り潰さずエラーとして返す（データを失わない）。
    pika::util::Result<ConfirmOutcome> confirm(pika::core::snapshot::SnapshotIndex& index,
                                               pika::core::workspace::UnreadSet& unread,
                                               const pika::core::document::ReviewTarget& target);

    // 「すべて確認済みにする」（design.md 5.4 / 要件8.3 /
    // E3）：開始時点の未読集合（targets）をフリーズ
    // して一括処理し、確認できたファイルのみ未読集合から除去する。退避失敗/並行変化でスキップした
    // ファイルは未読のまま残す（confirm_all の戻り値を握り潰さず skipped に伝える）。
    ConfirmAllOutcome confirm_all(pika::core::snapshot::SnapshotIndex& index,
                                  pika::core::workspace::UnreadSet& unread,
                                  const std::vector<pika::core::document::ReviewTarget>& targets,
                                  const std::string& batch_id, std::int64_t time);

    // 「すべて確認済み」の一括取消（design.md 5.4 / 要件8.3）：baseline-replace バッチを取り消し、
    // 取り消したファイルを未読集合へ戻す（ベースラインが旧内容へ戻る＝再び未読になる）。取消件数を返す。
    std::size_t revert_all(pika::core::snapshot::SnapshotIndex& index,
                           pika::core::workspace::UnreadSet& unread, const std::string& batch_id,
                           const std::vector<std::string>& confirmed_rel_paths);

    // 巻き戻しが提供可能か（design.md 5.4 / 要件8.3 / D3）。ReviewFlow に委譲。
    bool can_rollback(const pika::core::snapshot::SnapshotIndex& index, const std::string& rel_path,
                      const pika::core::document::FileContentClass& cls) const;

    // 「確認済み時点に戻す」（design.md 5.4 / 要件8.3）：現ディスク内容を rollback
    // 退避してベースライン 内容を返す（呼び出し側がディスクへ書き戻す）。退避を取れない対象は
    // Unsupported（握り潰さない）。
    pika::util::Result<std::string> rollback(pika::core::snapshot::SnapshotIndex& index,
                                             const pika::core::document::ReviewTarget& target);

    // 保存前チェック（design.md 5.3）：表現可能性 → 衝突検知（現ディスクハッシュ再計算）→ 衝突なら
    // incoming 退避 → 退避不能/退避失敗はブロック。Proceed
    // のときのみ呼び出し側がアトミック置換に進む。 退避結合の Result を握り潰さない（退避失敗を
    // BlockedStashFailed として返し上書きを止める）。
    SavePlan prepare_save(pika::core::snapshot::SnapshotIndex& index, const SaveContext& ctx);

  private:
    pika::core::snapshot::SnapshotStore& store_;
    pika::core::document::ReviewFlow flow_;
};

} // namespace pika::controller
