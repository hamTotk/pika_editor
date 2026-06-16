// controller/diff_mode_model: 差分モード状態機械（wx 非依存）。
// spec.md 系統A「差分モード状態機械」/ ui-design 8章（表示モードと差分）・design.md 6章
// （WebView2 1枚共有・占有世代）・4章（占有世代＝タブ/モード/差分トグル）/ spec.md sprint5 must。
//
// モード（ソース/分割/プレビュー。排他）と 差分トグル（ON/OFF。独立）を「直交」させ、各組合せでの
// 描画面構成（PaneLayout）を決定論的に解く。共有 1 枚 WebView2 への適用前照合に使う占有世代
// （タブ, モード, 差分ON の組）を算定する。巨大ファイル/WebView2 不在/ベースライン未取得では差分
// トグルを自動無効化し、その理由（ID）を返す。記号文字や日本語文言はここに散らさず列挙で返し、表示は
// GUI（系統B）が PaneLayout を機械的に消費する。純ロジック（同一入力で同一出力）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pika::controller
{

// 表示モード（排他。ui-design 8章。.md/.html/.svg で有効）。
enum class ViewMode
{
    Source,  // ソース（Scintilla 編集面）
    Split,   // 分割（エディタ＋プレビュー）
    Preview, // プレビュー（レンダリング）
};

// 描画面の構成要素（PaneLayout が組合せで持つ。GUI はこれを見てペインを出し分ける）。
// 「差分の描画面は常に WebView2 の差分HTML 1面」（ui-design 8章）。Scintilla にインライン差分を
// 作らない（軽量原則）。プレビュー＋差分ON のみ「1枚の WebView2 内で左プレビュー・右差分を grid」。
struct PaneLayout
{
    bool show_editor = false;       // Scintilla（ソース編集面）を出すか
    bool show_preview = false;      // WebView2 にレンダリングプレビューを出すか
    bool show_diff = false;         // WebView2 に差分HTML 面を出すか
    bool preview_diff_grid = false; // プレビュー＋差分ON＝1枚WebView2内に左右grid（design 6章）
    bool webview_active = false;    // この組合せで共有 WebView2 を占有するか（preview or diff）
};

// (モード × 差分ON/OFF) の直交組合せから描画面構成を解く（ui-design 8章の対応表）。
//   ソース  +差分OFF → エディタのみ
//   ソース  +差分ON  → 差分面のみ（実質差分ビュー。読み取り専用、編集は Ctrl+E でソースへ）
//   分割    +差分OFF → エディタ＋プレビュー
//   分割    +差分ON  → エディタ＋差分面
//   プレビュー+差分OFF → プレビューのみ
//   プレビュー+差分ON → 1枚WebView2内に左プレビュー・右差分を grid（preview_diff_grid）
PaneLayout resolve_pane_layout(ViewMode mode, bool diff_on);

// 差分トグルを無効化する理由（ui-design 8/15章 Partial。自動オフ＋理由表示）。
// None は差分トグル有効（押せる）。文言は diff_disable_reason_label が写す。
enum class DiffDisableReason
{
    None,            // 差分トグル有効
    FileTooLarge,    // 10MB 超（差分は計算せず自動オフ。要件8章）
    NoWebView,       // WebView2 不在（差分の描画面が無い。design 6章）
    NoBaseline,      // ベースライン未取得（前回確認時点が無く差分の基準が無い）
    NotDiffableType, // .md/.html/.svg 以外（差分面を出せる対象でない）
};

// 差分トグルの可否判定に必要な現在状況（GUI から集めて渡す。すべて wx 非依存の素値）。
struct DiffToggleContext
{
    bool diffable_type = true;     // 対象が差分可能な種別（.md/.html/.svg 等）か
    bool webview_available = true; // WebView2 ランタイムが使えるか（design 6章 2.3）
    bool has_baseline = false;     // ベースライン（前回確認時点）を持つか
    std::size_t content_bytes = 0; // 対象の現内容バイト数（10MB 判定）
    // 差分自動オフのサイズ閾値（既定 10MB。要件8章・設定で変更可なため注入する）。
    std::size_t max_diff_bytes = 10u * 1024u * 1024u;
};

// 差分トグルの可否を判定する純粋ロジック（ui-design 8章「巨大ファイル/WebView2不在/ベースライン
// 未取得では差分トグルを無効化＝自動オフ＋理由表示」）。優先順位は「種別 → WebView2 → ベースライン
// → サイズ。None なら押下可、それ以外は無効化理由を返す（通知バーから再有効化導線へ）。
DiffDisableReason evaluate_diff_toggle(const DiffToggleContext& ctx);

// 差分トグルが有効か（evaluate_diff_toggle(...) == None の薄いラッパ）。
bool diff_toggle_enabled(const DiffToggleContext& ctx);

// 無効化理由の日本語ラベル（通知バー文言。単一のメッセージ定義経由。design 10章 K9）。
std::string_view diff_disable_reason_label(DiffDisableReason reason);

// ---- 占有世代（design 4章・6章。共有 1 枚 WebView2 への結果適用前照合） ----

// WebView2 占有世代の鍵（タブ, モード, 差分ON）。ワーカー結果（差分計算・プレビュー変換）を
// 共有 WebView2 へ反映する前に「まだこの世代か」を照合し、別タブ/別モード/別差分状態へ切替済みなら
// 破棄する（design 4章「占有単位＝タブ/モード/差分トグル切替で +1」・5.5 手順3）。
struct OccupancyKey
{
    std::uint64_t tab_id = 0; // タブの同一性（消失/再オープンで使い回さない一意 ID）
    ViewMode mode = ViewMode::Source;
    bool diff_on = false;

    bool operator==(const OccupancyKey& o) const noexcept
    {
        return tab_id == o.tab_id && mode == o.mode && diff_on == o.diff_on;
    }
    bool operator!=(const OccupancyKey& o) const noexcept { return !(*this == o); }
};

// 共有 WebView2 の占有世代を数える状態機械。
// タブ/モード/差分ON のいずれかが変わるたび generation() を +1 する（切替軸に差分ON を含める）。
// ワーカーは結果に「投入時の generation」を添えて返し、UI は適用前に current() と一致確認する。
class OccupancyTracker
{
  public:
    // 現在の占有鍵を新しい鍵へ切り替える。鍵が変化したら世代を +1 して true を返す
    // （同一鍵への再要求は世代を進めない＝再ナビゲートを避ける。design 6章 (2) キャッシュ）。
    bool occupy(const OccupancyKey& key);

    // 投入時世代 stamp が現在世代と一致し、かつ鍵 key が現占有と一致するか（ワーカー結果の
    // 適用可否。design 5.5 手順3「完了前に別タブ/別モードへ切替済みなら破棄」）。
    bool is_current(std::uint64_t stamp, const OccupancyKey& key) const noexcept;

    std::uint64_t generation() const noexcept { return generation_; }
    const OccupancyKey& current() const noexcept { return current_; }

  private:
    OccupancyKey current_;
    std::uint64_t generation_ = 0;
};

} // namespace pika::controller
