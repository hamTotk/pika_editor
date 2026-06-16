// controller/tree_view_model: ツリー → ViewModel 変換（sprint1）。
// spec.md 系統A「ツリー → ViewModel 変換」/ ui-design.md
// 5章（状態記号）・6章（ファイルタイプアイコン）。
//
// wx 非依存。core/workspace の build_tree() 結果（TreeNode 木）と UnreadSet（未読集合）を入力に、
// 各ノードへ「状態マーク種別（±/◆/取消線・伝播
// ±淡）」と「種別アイコンカテゴリ」を決定論的に付与した
// 表示用の木（TreeRowVm）を組み立てる。純粋関数（同一入力で同一出力）であり、UI（wxDataViewCtrl）は
// この結果を描画するだけにする。記号の実体文字や日本語文言は本ヴューモデルに散らさず、列挙値として返す
// （表示文字列への写像は tree_view_messages が担う。design 10章 K9）。
#pragma once

#include "core/workspace/workspace_model.h"

#include <string>
#include <string_view>
#include <vector>

namespace pika::controller
{

// ノードに付ける状態マーク（ui-design 5章「状態記号の体系」）。
// 表示は「色＋記号」で行うが、ここでは色に依存しない種別だけを持つ（色だけに依存しない原則）。
// 重畳時の表示優先は Deleted > Unsaved > Diff（ui-design 5章・要件5.3）。本 sprint の入力は
// build_tree()+UnreadSet（外部変更＝差分あり/新規/伝播）であり、Deleted/Unsaved はタブ側の状態
// （sprint2 の TabManager）から合流するため、ここでは合成のための優先解決ロジックを公開する。
enum class StateMark
{
    None,           // マークなし
    Diff,           // 差分あり（±・accent）＝ベースラインから変化したファイル自身
    New,            // 新規ファイル（◆・diff-add）＝ベースラインなしで未読
    Deleted,        // 削除済み（取り消し線）＝外部削除＋バッファ保持
    Unsaved,        // 未保存（●・accent）＝エディタ編集の dirty
    DiffPropagated, // 配下に差分あり（±淡・text-3）＝子孫の差分をフォルダへ伝播
};

// 種別アイコンのカテゴリ（ui-design 6章の表。criteria の語彙に合わせる）。
// 未知拡張子は Unknown（generic file）へフォールバックする。
enum class IconCategory
{
    Folder,  // フォルダ
    Code,    // コード/マークアップ（ts/js/html/htm/xml）
    Data,    // データ（json）
    Config,  // 設定（yaml/toml/ini）
    Script,  // スクリプト（sh/ps1/bat）
    Image,   // 画像（png/jpg/gif/webp/bmp/ico/svg）
    Text,    // テキスト/文書（md/markdown/txt/csv/log）
    Unknown, // 不明（その他＝generic file）
};

// ファイル状態の素材（このノード自身の状態）。TreeViewModel への入力補助として、
// タブ側から来る上書き状態（削除済み・未保存）を畳み込むための最小フラグ集合。
// 本 sprint の build_tree()+UnreadSet 経路では deleted/unsaved は常に false で、sprint2 以降の
// TabManager が該当ファイルに対してセットする（重畳優先の解決はここで一元化する）。
struct NodeStateInput
{
    bool has_baseline = true; // ベースラインを持つか（false かつ未読＝新規 ◆）
    bool unread = false;      // ファイル自身が未読（差分あり）
    bool deleted = false;     // 外部削除＋バッファ保持（取り消し線）
    bool unsaved = false;     // エディタ編集の dirty（●）
};

// 表示用ツリー行（ViewModel）。core/workspace の TreeNode に表示属性を載せたもの。
struct TreeRowVm
{
    std::string name;                          // 表示名（セグメント名。TreeNode.name をそのまま）
    std::string rel_path;                      // ルート起点の相対パス（'/' 区切り）
    bool is_dir = false;                       // フォルダなら true
    StateMark mark = StateMark::None;          // 状態マーク種別（色非依存）
    IconCategory icon = IconCategory::Unknown; // 種別アイコンカテゴリ
    std::vector<TreeRowVm> children;           // フォルダ先行・自然順（build_tree の整列を保つ）
};

// ファイル状態の重畳を表示優先（削除済み ＞ 未保存 ＞ 差分あり）で 1 つの StateMark へ畳み込む
// （ui-design 5章・要件5.3）。新規（◆）は「差分あり」カテゴリの内部分岐（ベースラインなし）として
// 扱い、Diff より弱くはしない（未読の素材が無ければ None）。純粋関数。
StateMark resolve_file_mark(const NodeStateInput& s);

// 拡張子（'.' を含まない／含む小文字いずれも可）からアイコンカテゴリを写像する（ui-design 6章）。
// ファイル名でも拡張子でも受け取れるよう、最後の '.' 以降を拡張子と解釈する。未知は Unknown。
IconCategory classify_icon(std::string_view name_or_ext);

// core/workspace の TreeNode 木と UnreadSet から ViewModel 木を構築する純粋関数。
// - ファイルノード: classify_icon でアイコン分類し、UnreadSet（と新規ベースライン情報）から
//   resolve_file_mark で状態マークを決める。
// - フォルダノード: 自身に未読は付かない。子孫に差分あり（未読ファイル）があれば DiffPropagated。
//   ただし「自身に差分があるファイルの ±（実心）」と「フォルダの伝播 ±（淡）」は StateMark の
//   別値（Diff / DiffPropagated）で区別する（criteria）。
//
// new_files: ベースラインを持たない未読ファイル（新規 ◆）の相対パス集合。空なら全未読を ±（Diff）と
// 見なす。本 sprint では呼び出し側（WorkspaceController・sprint2
// 以降）が判定して渡す純粋入力にする。
TreeRowVm build_tree_view_model(const core::workspace::TreeNode& root,
                                const core::workspace::UnreadSet& unread,
                                const std::vector<std::string>& new_files = {});

} // namespace pika::controller
