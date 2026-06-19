// ui/image_view_panel: 画像簡易ビューア（wxPanel＝上部トグル＋自前描画スクロールキャンバス）。
// 要件12.2（画像簡易ビュー）/ design.md 10章 B3「画像簡易ビュー（WebView2 を起動しない）」/
// CLAUDE.md 補助原則「ネイティブ優先（WebView2 はプレビューと差分のみ）」。
//
// EditorPanel ではない読み取り専用ビュー（保存/dirty/差分の対象外）。MainFrame は notebook の
// 1 ページとして開き、dynamic_cast<EditorPanel*> が nullptr を返すため既存の保存/差分/dirty 経路は
// 自然に no-op になる（active_editor()==nullptr）。
//
// 表示は内側キャンバス（ImageCanvas）の OnPaint で wxImage→wxBitmap を描く（WebView2 不使用）。
// フィット（ウィンドウに合わせてアスペクト維持で縮小）と等倍（100%＋スクロール）を上部のトグル
// ボタンで切り替える。読み込み失敗時はその旨を中央に表示する（落ちない＝設計原則1）。
#pragma once

#include <wx/panel.h>

#include <string>

class wxButton;

namespace pika::ui
{

class ImageCanvas; // 自前描画のスクロールキャンバス（実装ファイル内で完結）

class ImageViewPanel : public wxPanel
{
  public:
    // path（絶対パス）の画像を読み込んで表示する。読み込み失敗は中央文言で示す（落ちない）。
    ImageViewPanel(wxWindow* parent, const std::string& path);

  private:
    void on_toggle_fit(wxCommandEvent& evt);

    ImageCanvas* canvas_ = nullptr;
    wxButton* toggle_button_ = nullptr;
};

} // namespace pika::ui
