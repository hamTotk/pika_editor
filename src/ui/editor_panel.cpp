#include "ui/editor_panel.h"

#include <wx/string.h>

namespace pika::ui
{

namespace
{

int to_sci_eol(controller::EolMode mode)
{
    switch (mode)
    {
    case controller::EolMode::Crlf:
        return wxSTC_EOL_CRLF;
    case controller::EolMode::Lf:
    case controller::EolMode::Mixed:
    default:
        // 混在は統一しない（要件5.2）。新規行挿入の既定だけ LF にし、既存の改行は触らない。
        return wxSTC_EOL_LF;
    }
}

} // namespace

EditorPanel::EditorPanel(wxWindow* parent, wxWindowID id)
    : wxStyledTextCtrl(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    // 内部表現を UTF-8 に固定する。core/document の content（UTF-8）をそのまま扱うため。
    SetCodePage(wxSTC_CP_UTF8);
    // 行番号余白（design 10章のエディタ体裁。最小骨格）。
    SetMarginType(0, wxSTC_MARGIN_NUMBER);
    SetMarginWidth(0, 40);
    // Scintilla の savepoint 通知で dirty 変化を拾う（GetModify をポーリングせず idiomatic に）。
    // 編集で savepoint を離れたら未保存、undo 等で savepoint に戻ったらクリーンに復帰する。
    Bind(wxEVT_STC_SAVEPOINTLEFT, &EditorPanel::on_save_point_left, this);
    Bind(wxEVT_STC_SAVEPOINTREACHED, &EditorPanel::on_save_point_reached, this);
}

void EditorPanel::apply_config(const controller::EditorConfig& cfg)
{
    // タブ/インデント設定を明示し原文を変えない（design 10章 G3）。SetUseTabs(false) でも既存の
    // タブ文字は空白へ変換しない（Tab キー入力時の挙動だけが変わる）。
    SetTabWidth(cfg.tab_width);
    SetUseTabs(cfg.use_tabs);
    SetIndent(cfg.tab_width);
    SetEOLMode(to_sci_eol(cfg.eol_mode));
    SetWrapMode(cfg.word_wrap ? wxSTC_WRAP_WORD : wxSTC_WRAP_NONE);
    // 空白/タブの可視化（要件11章）。可視化は表示のみで内容は変えない。
    SetViewWhiteSpace(cfg.show_whitespace ? wxSTC_WS_VISIBLEALWAYS : wxSTC_WS_INVISIBLE);
    SetReadOnly(cfg.read_only);
}

void EditorPanel::set_text_utf8(const std::string& utf8)
{
    const bool was_read_only = GetReadOnly();
    if (was_read_only)
    {
        SetReadOnly(false);
    }
    // wxString::FromUTF8 で UTF-8 バイト列を解釈する（CP_ACP を介さない）。改行は変換しない。
    SetText(wxString::FromUTF8(utf8.c_str(), utf8.size()));
    EmptyUndoBuffer(); // 読み込みは Undo 履歴の起点（外部変更反映の単一 Undo は sprint4）。
    SetSavePoint();    // 読み込み直後は未編集（dirty=false）。
    if (was_read_only)
    {
        SetReadOnly(true);
    }
}

void EditorPanel::reload_text_utf8(const std::string& utf8)
{
    const bool was_read_only = GetReadOnly();
    if (was_read_only)
    {
        SetReadOnly(false);
    }
    // 全文置換を単一 Undo にまとめる（EmptyUndoBuffer は呼ばない＝Ctrl+Z で旧内容へ戻せる）。
    BeginUndoAction();
    SetText(wxString::FromUTF8(utf8.c_str(), utf8.size()));
    EndUndoAction();
    // ディスク一致＝クリーン（dirty=false）。Ctrl+Z で旧内容へ戻すと自然に dirty になり ● が付く。
    SetSavePoint();
    if (was_read_only)
    {
        SetReadOnly(true);
    }
}

std::string EditorPanel::text_utf8() const
{
    // SCI の内部表現は UTF-8（CP_UTF8）。改行・空白を変換せず原文のまま取り出す。
    const wxString text = GetText();
    const wxScopedCharBuffer utf8 = text.utf8_str();
    return std::string(utf8.data(), utf8.length());
}

bool EditorPanel::is_dirty() const
{
    return GetModify();
}

std::int64_t EditorPanel::caret_position() const
{
    return GetCurrentPos();
}

void EditorPanel::set_caret_position(std::int64_t pos)
{
    // 範囲外は Scintilla が安全に丸める（GotoPos は内部で文書長へクランプする）。
    GotoPos(static_cast<int>(pos));
}

std::int64_t EditorPanel::first_visible_line() const
{
    return GetFirstVisibleLine();
}

void EditorPanel::set_first_visible_line(std::int64_t line)
{
    SetFirstVisibleLine(static_cast<int>(line));
}

void EditorPanel::goto_line(int line_1based, int column_1based)
{
    if (line_1based <= 0)
    {
        return; // 指定なし（0 始まりへの変換で負になる）は何もしない。
    }
    const int line0 = line_1based - 1; // Scintilla は 0 始まり。範囲外は SCI が安全に丸める。
    if (column_1based > 0)
    {
        // 桁指定あり: 行頭からの桁位置へキャレットを置く（行頭 + (col-1)）。FindColumn は
        // タブ幅を考慮しつつ行末を超えない位置へ丸めるため、範囲外でも落ちない。
        const int pos = FindColumn(line0, column_1based - 1);
        GotoPos(pos);
    }
    else
    {
        GotoLine(line0); // 行頭へ移動。
    }
    // 折りたたみ下でも当該行を可視化し、画面内へスクロールして入れる（行確認用途・要件3.1）。
    EnsureVisibleEnforcePolicy(line0);
    ScrollToLine(line0);
}

void EditorPanel::set_on_dirty_changed(std::function<void(bool)> cb)
{
    on_dirty_changed_ = std::move(cb);
}

void EditorPanel::mark_clean()
{
    // 保存成功後にクリーン状態を確定する。SAVEPOINTREACHED が発火し dirty=false が伝わる。
    SetSavePoint();
}

void EditorPanel::on_save_point_left(wxStyledTextEvent&)
{
    if (on_dirty_changed_)
    {
        on_dirty_changed_(true);
    }
}

void EditorPanel::on_save_point_reached(wxStyledTextEvent&)
{
    if (on_dirty_changed_)
    {
        on_dirty_changed_(false);
    }
}

} // namespace pika::ui
