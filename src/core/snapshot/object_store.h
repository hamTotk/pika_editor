// core/snapshot: 内容 object の格納庫（content-addressed・zstd 圧縮・自己記述サイドカー）。
// design.md 7章「objects\<hash> … 内容ハッシュ名で格納・重複排除」/ 要件9.1。
// サイドカーに自己記述メタを併記する。UI 非依存・実 FS を触る（gtest はテンポラリのデータルート）。
//
// object 名は内容（LF 正規化前の原文）の XXH3-64 hex。同一内容は 1 つの object を共有する
// （重複排除）。退避 object には自己記述サイドカー（元 relPath・kind・時刻・index 世代）を併記し、
// index.json 破損時に objects 走査だけで「復元待ち退避一覧」を再構築できるようにする（D1）。
#pragma once

#include "core/snapshot/snapshot_types.h"
#include "util/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pika::core::snapshot
{

// objects から走査で復元したサイドカーメタ 1 件（index.json 破損時の復元待ち一覧の要素）。
struct RecoveredStash
{
    std::string object_hash; // 内容 object 名
    std::string rel_path;    // 元の相対パス
    StashKind kind = StashKind::Conflict;
    std::int64_t time = 0;      // 退避時刻（Unix epoch 秒）
    std::int64_t index_gen = 0; // 退避時の index 世代（復元順の手掛かり）
    std::string batch_id;       // baseline-replace バッチ ID（他種別は空）
};

// 1 ワークスペース分の objects フォルダを管理する。コンストラクタはディレクトリを作らない
// （初回 put 時に遅延作成する。CLAUDE.md「軽い」）。
class ObjectStore
{
  public:
    // objects_dir はワークスペースキー配下の "objects" フォルダの絶対パス（UTF-8）。
    explicit ObjectStore(std::string objects_dir);

    // content を圧縮して内容ハッシュ名の object に格納し、その hash（XXH3-64 hex）を返す。
    // 既に同一 object があれば書かずに hash だけ返す（重複排除）。
    pika::util::Result<std::string> put(std::string_view content);

    // 退避 content を格納し、自己記述サイドカー（.meta）も併記する。返り値は object hash。
    // 同一内容で複数回退避された場合も object は共有し、サイドカーは最新の退避メタで上書きする。
    pika::util::Result<std::string> put_stash(std::string_view content, const std::string& rel_path,
                                              StashKind kind, std::int64_t time,
                                              std::int64_t index_gen, const std::string& batch_id);

    // hash の object を展開して内容を返す（無ければ NotFound）。
    pika::util::Result<std::string> get(const std::string& hash) const;

    // hash の object が存在するか。
    bool contains(const std::string& hash) const;

    // hash の object と（あれば）サイドカーを物理削除する。共有実体の誤削除防止は呼び出し側
    // （snapshot_store の mark-and-sweep）が担うため、本メソッドは無条件に消す。
    void remove(const std::string& hash);

    // objects フォルダ内に実在する全 object hash を列挙する（mark-and-sweep の sweep 対象）。
    std::vector<std::string> list_objects() const;

    // サイドカー（.meta）を走査して復元待ち退避一覧を返す（index.json 破損時の復元経路。D1）。
    // 対応する object が現存するものだけを返す（object を伴わないメタは復元不能のため除外）。
    std::vector<RecoveredStash> scan_recoverable_stashes() const;

    const std::string& dir() const noexcept { return dir_; }

    // object hash として妥当か（XXH3-64 = 16 桁の小文字 16 進数）。index.json/サイドカー由来の
    // hash が objects フォルダ外へ相対参照しないことを、パス合成前に保証する許可リスト検証
    // （パストラバーサル多層防御）。put/put_stash は内容から内部計算するため常に妥当。
    static bool is_valid_hash(const std::string& hash) noexcept;

  private:
    std::string object_path(const std::string& hash) const;
    std::string meta_path(const std::string& hash) const;

    std::string dir_;
};

} // namespace pika::core::snapshot
