// ui/unsupported_view_panel: 非対応/巨大画像ビュー（中央ラベル＋「既定のアプリで開く」）。
// 要件12.2（I2 巨大画像ガード・I9 バイナリ非対応表示）/ design.md 10章 B3 /
// ui-design 15章（Error/Partial＝機能縮退で行き止まりにしない＝次の一手を提示）。
//
// バイナリ（NUL を含む等の非テキスト）と巨大画像（総ピクセル数ガード超過＝デコードしない）の両方で
// 共通利用する読み取り専用ビュー。EditorPanel ではないため保存/dirty/差分は自然に no-op になる。
// 中央に種別ラベル＋説明、その下に「既定のアプリで開く」ボタンを置き、ユーザーを行き止まりにしない。
#pragma once

#include <wx/panel.h>

#include <string>

namespace pika::ui
{

class UnsupportedViewPanel : public wxPanel
{
  public:
    // path（絶対パス・既定のアプリで開く対象）と表示文言（種別ラベル・説明）を受け取る。
    // title は degrade_kind_label（巨大画像）or「対応していない形式です」（バイナリ）を呼び出し側で
    // 解決して渡す（文言は単一メッセージ定義経由。design 10章 K9）。すべて UTF-8。
    UnsupportedViewPanel(wxWindow* parent, const std::string& path, const std::string& title,
                         const std::string& detail);

  private:
    void on_open_default(wxCommandEvent& evt);

    std::string path_; // 「既定のアプリで開く」対象（絶対パス）
};

} // namespace pika::ui
