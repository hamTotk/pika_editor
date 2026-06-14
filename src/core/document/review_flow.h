// core/document: 退避フロー結合（DocumentController × snapshot のコアロジック）。
// design.md 5.3 保存／5.4 既読化／7章 退避フロー / 要件7.3・8.3・9章。
// spec.md「中心体験」4「前回確認時点からの累積差分を確認し、確認済みにする」5「軽く修正して保存」。
// 設計原則1「データを失わない」が最優先（退避＝最後の砦。破壊的操作の前に退避を取る）。
//
// 本クラスは中心体験の衝突・巻き戻し・確認済みを「ディスク内容（std::string）」を入力にした純
// オーケストレーションとして実装する。実 FS への読み書きは SnapshotStore（退避・ベースライン）と
// 呼び出し側 DocumentController（タブ内容・ディスク反映）が担い、本層は wx・WebView2・Win32 を
// 一切含まない（gtest で決定論検証できる。design.md 13章「自動単体テストの対象は core/・util」）。
//
// content_object_allowed=false（10MB以上・画像）と sensitive（.env 等）は内容 object を持たない
// ため、差分・巻き戻しを非活性とし、破壊的操作の前に退避を取れない（退避不能ガード。D2/D3）。
#pragma once

#include "core/snapshot/snapshot_store.h"
#include "core/snapshot/snapshot_types.h"
#include "util/result.h"

#include <string>
#include <string_view>
#include <vector>

namespace pika::core::document
{

// 1 ファイルの内容種別。content_object_allowed=false（10MB以上・画像）と sensitive は内容 object を
// 持たないため、差分・巻き戻し・破壊的操作前の退避ができない（退避不能。要件9.2/D2/D3）。
struct FileContentClass
{
    bool sensitive = false;             // 機密（.env/*.key/*.pem/*secret*）＝ハッシュのみ
    bool content_object_allowed = true; // 内容 object を持てるか（false=10MB以上・画像）

    // 退避・差分・巻き戻しを行える内容を保持できるか（内容 object を持てるかと等価）。
    bool can_stash() const noexcept { return content_object_allowed && !sensitive; }
};

// 「確認済みにする」「巻き戻し」「すべて確認済み」操作の対象 1 件。
// content は呼び出し側がディスク（または差分計算に使ったスナップショット）から読んだ現内容。
struct ReviewTarget
{
    std::string rel_path;
    std::string content;    // 現ディスク内容（原文。確認済み・退避の入力）
    std::int64_t mtime = 0; // 確認時の mtime（ベースライン再判定・起動プレスクリーン用）
    FileContentClass cls;   // 内容種別（退避可否の判定に使う）

    // confirm_all（すべて確認済み）でのみ使う「フリーズ時点の内容ハッシュ」（LF 正規化 XXH3 hex）。
    // 開始時点の未読集合をフリーズした時の content のハッシュ。処理中に並行書き込みで content が
    // 変わったか（フリーズ時と現在で不一致）を検知してスキップするための基準（要件8.3 / E3）。
    // 空のときは並行変化チェックを行わない（confirm 等この基準を使わない経路では未設定でよい）。
    std::string freeze_hash;
};

// 「すべて確認済み」の結果（処理中に変化したファイルのスキップと一括取消バッチを伝える）。
struct AllConfirmedResult
{
    std::string batch_id;               // 一括取消の単位（baseline-replace バッチ）
    std::vector<std::string> confirmed; // ベースラインを更新した rel_path
    std::vector<std::string> skipped;   // 開始時と内容が変わりスキップした rel_path（未読のまま）
};

// 退避フロー結合のオーケストレータ。SnapshotStore を 1 ワークスペース分だけ束ねる。
// index は呼び出し側（DocumentController）が load→操作→save する所有モデルに合わせ、各メソッドは
// index への参照を受け取り破壊的に更新する（SnapshotStore と同一規約。経路乖離を防ぐ）。
class ReviewFlow
{
  public:
    explicit ReviewFlow(pika::core::snapshot::SnapshotStore& store) : store_(store) {}

    // 保存時衝突（要件7.3）：現ディスク内容（外部に書き換えられた incoming）を退避してから上書き
    // 保存する経路。退避が取れない対象（can_stash=false）は退避不能ガードで既定ブロック（Unsupported）。
    // 戻り値は退避エントリ（hash で内容を復元できる）。実際の保存は呼び出し側が続けて行う。
    pika::util::Result<pika::core::snapshot::StashEntry> stash_incoming_before_save(
        pika::core::snapshot::SnapshotIndex& index, const std::string& rel_path,
        std::string_view disk_content, const FileContentClass& cls, std::int64_t time);

    // 取り込み時（要件7.3）：外部変更を取り込む前に自分の未保存編集（conflict）を退避する経路。
    // disk_content は取り込む外部内容ではなく、退避する「自分の編集内容」。
    pika::util::Result<pika::core::snapshot::StashEntry> stash_conflict_before_take(
        pika::core::snapshot::SnapshotIndex& index, const std::string& rel_path,
        std::string_view my_edit, const FileContentClass& cls, std::int64_t time);

    // 「確認済みにする」（要件8.3 / 5.4）：ディスク内容でベースラインを更新し未読を解除する。
    // 内容を持たない（10MB以上・画像・機密）ファイルはハッシュベースラインのみ更新する（D3）。
    // 戻り値は更新後の IndexEntry。
    pika::util::Result<pika::core::snapshot::IndexEntry> confirm(
        pika::core::snapshot::SnapshotIndex& index, const ReviewTarget& target);

    // 巻き戻しが提供可能か（要件8.3 / D3）。ベースライン内容 object を持つ（can_stash かつ
    // baseline_object 非空）場合のみ true。10MB以上・画像・機密・ベースライン未取得は
    // false（非活性）。
    bool can_rollback(const pika::core::snapshot::SnapshotIndex& index, const std::string& rel_path,
                      const FileContentClass& cls) const;

    // 「確認済み時点に戻す」（要件8.3）：現在のディスク内容を rollback
    // 退避に保存し、ベースライン内容を 返す（呼び出し側がディスクへ書き戻す）。退避を取れない対象は
    // can_rollback=false で Unsupported。 戻り値はベースライン内容（巻き戻し先）。
    pika::util::Result<std::string> rollback(pika::core::snapshot::SnapshotIndex& index,
                                             const ReviewTarget& target);

    // 「すべて確認済みにする」（要件8.3 / 5.4 / E3）：開始時点の未読集合（targets）をフリーズし、各
    // ファイルの旧ベースラインを baseline-replace 退避（同一 batch_id）してから更新する。
    // 各 target の freeze_hash（フリーズ時点の内容ハッシュ）と現内容のハッシュが不一致なら、処理
    // 中に並行書き込みで変わった＝未確認内容なのでスキップして未読のまま残す。完了後に一括取消できる。
    AllConfirmedResult confirm_all(pika::core::snapshot::SnapshotIndex& index,
                                   const std::vector<ReviewTarget>& targets,
                                   const std::string& batch_id, std::int64_t time);

    // confirm_all の baseline-replace
    // バッチを一括取消する（旧ベースラインを復元する起点。SnapshotStore に委譲）。取消件数を返す。
    std::size_t revert_all(pika::core::snapshot::SnapshotIndex& index, const std::string& batch_id);

  private:
    pika::core::snapshot::SnapshotStore& store_;
};

} // namespace pika::core::document
