// ui/editor_panel: Scintilla（wxStyledTextCtrl）を 1 タブ分ラップする UI 部品。
// design.md 5.3・10章 G3「タブ/インデント設定を明示し原文を変えない」/ 要件5.1・5.2 / spec.md
// sprint3 must（Scintilla 配線・エンコーディング/改行結果の反映）。
//
// 配線判断（どの設定をどの SCI パラメータへ流すか）は controller::make_editor_config（wx 非依存・
// gtest 済み）が決め、本クラスはその EditorConfig を SCI_SETTABWIDTH/SCI_SETUSETABS/SCI_SETEOLMODE
// へ機械的に流すだけにする。表示内容は core/document（decode_auto）が UTF-8 へ正規化した content を
// そのまま受け取り、改行・空白を変換しない（原文維持）。
#pragma once

#include "controller/editor_view_model.h"

#include <wx/stc/stc.h>
#include <wx/window.h>

#include <cstdint>
#include <functional>
#include <string>

namespace pika::ui
{

class EditorPanel : public wxStyledTextCtrl
{
  public:
    EditorPanel(wxWindow* parent, wxWindowID id = wxID_ANY);

    // EditorConfig（controller の決定値）を Scintilla パラメータへ適用する。原文は変えない。
    void apply_config(const controller::EditorConfig& cfg);

    // UTF-8 content を表示する（改行・空白を変換しない）。読み込み直後は未編集（dirty クリア）。
    void set_text_utf8(const std::string& utf8);

    // 外部変更を反映して再読込する（D3 ライブリロード）。set_text_utf8 と違い Undo 履歴を消さず
    // 全文置換を単一 Undo にまとめる（Ctrl+Z で旧内容へ戻せる）。直後は SetSavePoint でクリーン。
    void reload_text_utf8(const std::string& utf8);

    // 現在のバッファ内容を UTF-8 で取り出す（プレビュー/差分のソース。sprint5）。
    std::string text_utf8() const;

    // 編集の有無（未保存判定。SCI のモディファイ状態）。
    bool is_dirty() const;

    // キャレット位置・先頭可視行の取得/設定（state.json 復元。要件10.1・F1）。範囲外は Scintilla が
    // 安全に丸めるため、復元前に内容を当てておけば不正値でも落ちない（データを失わない・設計原則1）。
    std::int64_t caret_position() const;
    void set_caret_position(std::int64_t pos);
    std::int64_t first_visible_line() const;
    void set_first_visible_line(std::int64_t line);

    // 指定行（1 始まり）へキャレットを移動し、その行を表示範囲へ収める（-g 行ジャンプ。要件3.1）。
    // column（1 始まり・0 は指定なし）があれば行頭からの桁位置へ寄せる。範囲外は Scintilla が
    // 安全に丸める。line_1based <= 0 は何もしない。
    void goto_line(int line_1based, int column_1based);

    // dirty 変化の通知コールバック（true=未保存になった/false=クリーンに戻った）。
    // Scintilla の savepoint 通知（SAVEPOINTLEFT/SAVEPOINTREACHED）から呼ぶ。
    // MainFrame が TabManager の未保存フラグ・タブ記号へ結線する（design 5.3・要件5.3）。
    void set_on_dirty_changed(std::function<void(bool)> cb);

    // 保存成功後にクリーン状態を確定する（SCI の savepoint を現在位置へ張り直す）。
    // 以後の編集で再び SAVEPOINTLEFT が発火し、未保存記号が付くようにする。
    void mark_clean();

  private:
    // 編集で savepoint を離れた＝dirty。
    void on_save_point_left(wxStyledTextEvent& evt);
    // undo 等で savepoint に戻った＝clean。
    void on_save_point_reached(wxStyledTextEvent& evt);

    std::function<void(bool)> on_dirty_changed_;
};

} // namespace pika::ui
