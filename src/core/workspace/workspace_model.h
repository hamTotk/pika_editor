// core/workspace: フォルダ状態（ツリーモデル・未読集合・除外リスト適用・rename引き継ぎ）。
// design.md 2章 workspace（`WorkspaceModel`/`UnreadSet`）/ 要件4章（ツリーと未読）。
//
// UI 非依存。ディスク列挙の結果（パス一覧）を受け取り、フォルダ先行・自然順ソート（要件4.1）、
// 既定除外（.git/node_modules。要件4.1）、未読集合と子孫伝播（要件4.2）、rename/移動での
// 未読・ベースライン・退避の引き継ぎ（要件4.2/7.2）を純ロジックで提供する。実 FS の監視・列挙は
// 上位（WorkspaceController）が行い、本モジュールはそのスナップショットに対する決定論的な計算を担う。
//
// パスは相対パス（ワークスペースルート起点）・'/'
// 区切りに正規化されている前提（呼び出し側で正規化）。
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pika::core::workspace
{

// 自然順比較（要件4.1「file2 が file10 より前」）。
// 連続する数字列を数値として比較し、それ以外は大小無視の辞書順で比較する。a<b で true。
bool natural_less(std::string_view a, std::string_view b);

// パスが除外リストのいずれかに該当するか（要件4.1）。
// パスのいずれかのセグメント名（ディレクトリ名・ファイル名）が exclude の要素と一致すれば true。
// 例: ".git" は ".git/config" を、"node_modules" は "a/node_modules/b.js" を除外する。
bool is_excluded(std::string_view rel_path, const std::vector<std::string>& exclude);

// ツリーノード（フォルダ/ファイル）。子はフォルダ先行・自然順で整列済み。
struct TreeNode
{
    std::string name;     // セグメント名（表示名）
    std::string rel_path; // ルート起点の相対パス（'/' 区切り）
    bool is_dir = false;  // フォルダなら true
    std::vector<TreeNode> children;
};

// 1 エントリ。is_dir はフォルダ判定。rel_path は '/' 区切りの相対パス。
struct Entry
{
    std::string rel_path;
    bool is_dir = false;
};

// entries（フォルダ＋ファイル混在の一覧）から除外を適用し、フォルダ先行・自然順のツリーを構築する。
// 入力順に依存せず決定論的に整列する。除外された配下は監視対象外として木に含めない。
TreeNode build_tree(const std::vector<Entry>& entries, const std::vector<std::string>& exclude);

// 未読集合（要件4.2）。ファイル自身の未読を保持し、フォルダの伝播未読を導出する。
// 永続側は index.json の unread フラグ（design.md 9章）。本クラスはメモリ側モデル。
class UnreadSet
{
  public:
    // ファイルを未読にする（外部変更・新規作成。要件4.2）。
    void mark(const std::string& rel_path);

    // 「確認済みにする」で未読を解除する（要件4.2/8章）。
    void clear(const std::string& rel_path);

    // ファイル自身が未読か。
    bool is_unread(std::string_view rel_path) const;

    // フォルダ rel_path の子孫に未読ファイルが 1 件でもあるか（伝播未読。要件4.2「折りたたみ中でも
    // 気づける」）。rel_path 自身は含めず、その配下（rel_path + "/" で始まるパス）を見る。
    // ルート全体は rel_path を空文字で呼ぶ。
    bool has_unread_descendant(std::string_view rel_path) const;

    // 未読ファイル総数（ステータスバー「フォルダ内の未読ファイル数」。要件11章）。
    std::size_t count() const noexcept { return unread_.size(); }

    const std::set<std::string>& items() const noexcept { return unread_; }

  private:
    std::set<std::string> unread_;
};

// rename/移動の 1 件（要件4.2/7.2）。old_path から new_path へ。
// new_path が空なら旧名単独（削除側）、old_path が空なら新名単独（新規側）を表す
// （watcher の rename ペア不成立の安全側正規化。design.md 5.2）。
struct RenameOp
{
    std::string old_path; // 旧相対パス（空＝新名単独）
    std::string new_path; // 新相対パス（空＝旧名単独）
};

// ファイルごとの引き継ぎ対象状態（要件4.2「未読・ベースライン・退避を引き継ぐ」）。
struct CarryState
{
    bool unread = false;                // 未読フラグ
    bool has_baseline = false;          // ベースラインを持つか
    std::uint64_t baseline_hash = 0;    // ベースラインの LF 正規化ハッシュ
    std::vector<std::string> stash_ids; // 紐づく退避（object）ID
};

// rename 引き継ぎの 1 件の結果分類（要件4.2 の安全側規則）。
enum class CarryOutcome
{
    Moved,        // old→new に状態を引き継いだ
    OverwroteDst, // new に既存エントリがあり、old の状態で上書きした
    Removed,      // 旧名単独＝削除（状態は孤立保全のため残す）
    Created,      // 新名単独＝新規（ベースラインなしで開始）
    Reevaluated,  // 対応付け不能（往復等）→最終ディスク内容で再判定する指示
};

// rename 適用の結果。各 op の分類と、更新後の状態マップを返す。
struct CarryResult
{
    std::vector<CarryOutcome> outcomes;       // ops と同順
    std::map<std::string, CarryState> states; // rel_path → 引き継ぎ後の状態
    // 対応付け不能で「最終ディスク内容で再判定」が必要なパス（要件4.2）。
    std::vector<std::string> reevaluate;
    // 引き継ぎに失敗し旧キーで孤立保全したパス（90日GC に委ねる。要件4.2/7.2）。
    std::vector<std::string> orphaned;
};

// states（rename 前の各ファイル状態）に ops を適用し、未読・ベースライン・退避を引き継ぐ。
// 引き継ぎ規則（要件4.2）:
//   - 通常の old→new: 状態を new へ移す（Moved）
//   - new に既存エントリがある: old の状態で上書きする（OverwroteDst）
//   - 相互スワップ（A↔B）: 一時退避を経てアトミックに付け替える（双方 Moved）
//   - 短時間の往復（A→B→A 等で対応付け確定不能）: reevaluate に積む（Reevaluated）
//   - 旧名単独: Removed（状態は orphaned で保全）／新名単独: Created（ベースラインなし）
CarryResult apply_renames(const std::map<std::string, CarryState>& states,
                          const std::vector<RenameOp>& ops);

} // namespace pika::core::workspace
