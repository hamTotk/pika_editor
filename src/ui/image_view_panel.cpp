#include "ui/image_view_panel.h"

#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <algorithm>

namespace pika::ui
{

// 画像を自前描画するスクロールキャンバス。フィット時はアスペクト維持で縮小し中央寄せ、等倍時は
// 原寸でスクロール可能にする。読み込み失敗時は中央にメッセージを描く（WebView2 を起動しない）。
class ImageCanvas : public wxScrolledWindow
{
  public:
    ImageCanvas(wxWindow* parent, const std::string& path) : wxScrolledWindow(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT); // 自前描画（ちらつき防止のバッファ DC）。
        SetScrollRate(16, 16);
        load(path);
        Bind(wxEVT_PAINT, &ImageCanvas::on_paint, this);
        Bind(wxEVT_SIZE, &ImageCanvas::on_size, this);
    }

    // フィット↔等倍を切り替えてスクロール領域を再計算する。
    void set_fit(bool fit)
    {
        fit_to_window_ = fit;
        update_virtual_size();
        Refresh();
    }

    bool fit_to_window() const { return fit_to_window_; }
    bool loaded() const { return image_.IsOk(); }

  private:
    void load(const std::string& path)
    {
        // wxLogNull で「ハンドラが無い」等の警告ダイアログを抑止する（落ちない・静かに失敗する）。
        wxLogNull no_log;
        // UTF-8 パスを正しくワイド化して読む（CP_ACP を介さない。util/path_util と同方針）。
        const wxString wpath = wxString::FromUTF8(path.c_str(), path.size());
        image_.LoadFile(wpath); // 形式は内容から自動判定（標準ハンドラ）。
    }

    void update_virtual_size()
    {
        if (!image_.IsOk())
        {
            SetVirtualSize(0, 0);
            return;
        }
        if (fit_to_window_)
        {
            // フィットはスクロール不要（ビューポートに収める）。
            SetVirtualSize(0, 0);
        }
        else
        {
            // 等倍は原寸ぶんのスクロール領域を確保する。
            SetVirtualSize(image_.GetWidth(), image_.GetHeight());
        }
    }

    void on_size(wxSizeEvent& evt)
    {
        if (fit_to_window_)
        {
            Refresh(); // フィット倍率はビューポートサイズに依存するため再描画する。
        }
        evt.Skip();
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        DoPrepareDC(dc);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const wxSize client = GetClientSize();
        if (!image_.IsOk())
        {
            // 読み込み失敗: 中央にメッセージ（クラッシュさせず状態を伝える）。
            const wxString msg = wxString::FromUTF8("画像を読み込めませんでした");
            const wxSize te = dc.GetTextExtent(msg);
            dc.DrawText(msg, (client.x - te.x) / 2, (client.y - te.y) / 2);
            return;
        }

        if (fit_to_window_)
        {
            // ビューポートにアスペクト維持で収める（拡大はせず縮小のみ＝1.0 を上限にする）。
            const double sx = static_cast<double>(client.x) / image_.GetWidth();
            const double sy = static_cast<double>(client.y) / image_.GetHeight();
            double scale = std::min(sx, sy);
            if (scale > 1.0)
            {
                scale = 1.0; // 小さい画像を粗く拡大しない（原寸まで）。
            }
            const int w = std::max(1, static_cast<int>(image_.GetWidth() * scale));
            const int h = std::max(1, static_cast<int>(image_.GetHeight() * scale));
            // 縮小は wxImage::Scale（HIGH 品質）で行い、その都度 bitmap
            // 化する（フィットは頻度低）。
            wxBitmap bmp(image_.Scale(w, h, wxIMAGE_QUALITY_HIGH));
            dc.DrawBitmap(bmp, (client.x - w) / 2, (client.y - h) / 2, true);
        }
        else
        {
            // 等倍: 原寸で左上から描く（スクロールは DoPrepareDC が原点をずらす）。
            wxBitmap bmp(image_);
            dc.DrawBitmap(bmp, 0, 0, true);
        }
    }

    wxImage image_;
    bool fit_to_window_ = true;
};

ImageViewPanel::ImageViewPanel(wxWindow* parent, const std::string& path)
    : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // 上部のトグルバー（等倍/フィットの切替）。最小実装＝ボタン 1 つ（要件のとおり）。
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    toggle_button_ = new wxButton(this, wxID_ANY, wxString::FromUTF8("等倍表示 (100%)"));
    bar->Add(toggle_button_, 0, wxALL, 4);
    sizer->Add(bar, 0, wxEXPAND);

    canvas_ = new ImageCanvas(this, path);
    sizer->Add(canvas_, 1, wxEXPAND);

    SetSizer(sizer);

    // 画像を読めなかったときは切替の意味がないためボタンを無効化する（誤操作の混乱を避ける）。
    if (!canvas_->loaded())
    {
        toggle_button_->Enable(false);
    }
    toggle_button_->Bind(wxEVT_BUTTON, &ImageViewPanel::on_toggle_fit, this);
}

void ImageViewPanel::on_toggle_fit(wxCommandEvent&)
{
    const bool next_fit = !canvas_->fit_to_window();
    canvas_->set_fit(next_fit);
    // ボタン文言は「次に切り替わる状態」を示す（フィット中は等倍へ、等倍中はフィットへ）。
    toggle_button_->SetLabel(next_fit ? wxString::FromUTF8("等倍表示 (100%)")
                                      : wxString::FromUTF8("ウィンドウに合わせる"));
}

} // namespace pika::ui
