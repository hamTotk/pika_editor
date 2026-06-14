// core/snapshot: ベースライン・退避の台帳データモデル（index.json のメモリ表現）。
// design.md 7章「index.json のエントリ」/ 要件9章。UI 非依存・wx 非依存（gtest 対象）。
//
// 退避（stash）は conflict/incoming/rollback/baseline-replace の 4 種で、内容実体は objects に
// 内容ハッシュ名で重複排除して格納する。本ヘッダは台帳の型のみを定義し、永続化・容量管理は
// snapshot_store が担う。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pika::core::snapshot
{

// 退避の種別（design.md 7章 / 要件9.2）。
//   Conflict        … ［取り込む］時に退避した自分の未保存編集（要件7.3）
//   Incoming        … 衝突を承知で上書き保存する際に退避した外部変更のディスク内容（要件7.3）
//   Rollback        … 巻き戻しで失われる直前の内容（要件8.3）
//   BaselineReplace … 「すべて確認済みにする」更新前の旧ベースライン（要件8.3）
enum class StashKind
{
    Conflict = 0,
    Incoming,
    Rollback,
    BaselineReplace,
};

// kind 文字列（index.json / サイドカーで使う安定表記）。
const char* to_string(StashKind kind) noexcept;
// 文字列から StashKind を復元する（未知文字列は false を返す）。
bool parse_stash_kind(const std::string& s, StashKind& out) noexcept;

// 退避エントリ。hash は退避内容（LF 正規化前の原文）の XXH3-64 hex（objects 名）。
struct StashEntry
{
    // 退避内容の object 名（XXH3-64 hex）。
    std::string hash;
    // 退避時刻（Unix epoch 秒）。LRU・90日/14日判定に使う。
    std::int64_t time = 0;
    StashKind kind = StashKind::Conflict;
    // baseline-replace の一括取消バッチ ID（他種別は空）。
    std::string batch_id;
    // 復元済みフラグ（未復元退避の保護判定に使う）。
    bool restored = false;
};

// ファイルごとの台帳エントリ（index.json の 1 件）。
struct IndexEntry
{
    // ワークスペース相対パス（単体ファイルは "file-<hash>" キー下の絶対パス相当）。
    std::string rel_path;
    // ベースライン内容の LF 正規化 XXH3-64 hex（必須）。
    std::string baseline_hash;
    // ベースライン内容の object 名（原文 XXH3-64 hex）。空＝ハッシュのみ記録。
    std::string baseline_object;
    // ベースライン取得時の mtime（起動時プレスクリーン用）。
    std::int64_t baseline_mtime = 0;
    // ベースライン取得時のサイズ（起動時プレスクリーン用）。
    std::uint64_t baseline_size = 0;
    // 未読フラグ。
    bool unread = false;
    // 退避一覧（ファイルごと最新10件 LRU ＋ baseline-replace バッチ）。
    std::vector<StashEntry> stash;
};

// ワークスペース 1 件分の台帳（index.json 全体のメモリ表現）。
struct SnapshotIndex
{
    // スキーマ version（単調増加。未知 version は安全側に倒す）。
    int version = 1;
    // ファイルごとのエントリ。
    std::vector<IndexEntry> entries;

    // rel_path で検索する（無ければ nullptr）。
    IndexEntry* find(const std::string& rel_path) noexcept;
    const IndexEntry* find(const std::string& rel_path) const noexcept;
};

// index.json の現行スキーマ version（破壊的変更時のみ上げる単調増加。design.md 7章 K2）。
inline constexpr int kIndexVersion = 1;

// 内容を保存する閾値（要件9.2 = 10MB 未満のテキストのみ内容 object を持つ）。
inline constexpr std::uint64_t kContentSizeLimit = 10ull * 1024 * 1024;

} // namespace pika::core::snapshot
