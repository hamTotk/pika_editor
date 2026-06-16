// controller/shortcut_table: フォーカス別ショートカット・ディスパッチ表（wx 非依存）。
// design.md 10章 J3「ショートカットはフォーカス別にディスパッチする：Ctrl+Enter は差分/プレビューに
// フォーカスがあるときのみ『確認済みにする』を発火し、エディタフォーカス時は Ctrl+Shift+Enter を
// 用いる。一括確認は Ctrl+Alt+Enter」/ 要件11.2 / spec.md sprint6 must（ショートカット割当表）。
//
// 同じ Enter 系キーでも、フォーカスがエディタにあるか差分/プレビューにあるかで意味が変わる
// （エディタで Ctrl+Enter は改行挿入＝確認済みではない）。この「フォーカス × 修飾キー →
// アクション」の 写像を 1 つの決定論テーブルへ集約し、GUI（系統B）はキーイベントを KeyChord
// へ写してこの表を引くだけに する（割当が各ハンドラへ散らばるのを防ぐ。判断ロジックを gtest
// で観測する）。
#pragma once

namespace pika::controller
{

// キー入力のフォーカス文脈（どのペインが入力フォーカスを持つか。design.md 10章 J3・F6 循環）。
enum class FocusContext
{
    Editor,   // Scintilla（ソース編集面）
    DiffView, // 差分ビュー（WebView2 の差分面）
    Preview,  // プレビュー（WebView2 のレンダリング面）
    Tree,     // ファイルツリー
    Other,    // 上記以外（通知バー等）
};

// 修飾キー付きの確定キー入力（GUI がキーイベントから組み立てる素値。wx 非依存）。
// 本 sprint の対象は Enter 系の確認ショートカットに限る（足さない）。
struct KeyChord
{
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool enter = false; // Enter（Return）キーか
};

// ディスパッチ先のアクション（GUI が実ハンドラへ振り分ける。文言・実処理は持たない）。
enum class ShortcutAction
{
    None,    // 割当なし（GUI は既定動作へ委ねる＝エディタの改行挿入等を奪わない）
    Confirm, // 「確認済みにする」（差分/プレビュー時の Ctrl+Enter・エディタ時の Ctrl+Shift+Enter）
    ConfirmAll, // 「すべて確認済みにする」一括（フォーカス非依存の Ctrl+Alt+Enter）
};

// フォーカス文脈とキー入力から発火すべきアクションを決める純粋関数（design.md 10章 J3）。
//   - Ctrl+Alt+Enter             → ConfirmAll（どのフォーカスでも一括確認。フォーカス非依存）
//   - Ctrl+Enter（差分/プレビュー） → Confirm
//   - Ctrl+Shift+Enter（エディタ）  → Confirm（エディタでは Ctrl+Enter を改行に譲るため Shift
//   を足す）
//   - 上記以外                     → None（既定動作を奪わない）
// 同一入力で同一出力（同じ割当を 2 経路で再実装しないための単一テーブル）。
ShortcutAction dispatch_shortcut(FocusContext focus, const KeyChord& chord);

} // namespace pika::controller
