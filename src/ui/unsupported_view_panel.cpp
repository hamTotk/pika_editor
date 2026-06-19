#include "ui/unsupported_view_panel.h"

#include "ui/ui_messages.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

namespace pika::ui
{

namespace
{

wxString u8(const std::string& s)
{
    return wxString::FromUTF8(s.c_str(), s.size());
}

} // namespace

UnsupportedViewPanel::UnsupportedViewPanel(wxWindow* parent, const std::string& path,
                                           const std::string& title, const std::string& detail)
    : wxPanel(parent, wxID_ANY), path_(path)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* title_text = new wxStaticText(this, wxID_ANY, u8(title), wxDefaultPosition, wxDefaultSize,
                                        wxALIGN_CENTRE_HORIZONTAL);
    // 種別ラベルは少し大きめに（情報の主従を視覚的に示す。ui-design 15章 Error/Empty）。
    wxFont title_font = title_text->GetFont();
    title_font.MakeLarger();
    title_font.MakeBold();
    title_text->SetFont(title_font);

    auto* detail_text = new wxStaticText(this, wxID_ANY, u8(detail), wxDefaultPosition,
                                         wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    // 説明は折り返して読めるようにする（長文でも横スクロールさせない）。
    detail_text->Wrap(420);

    auto* open_button = new wxButton(this, wxID_ANY, u8(message(MsgId::OpenInDefaultApp)));
    // 「既定のアプリで開く」が無効にならないよう、開く対象が無いときだけ無効化する。
    open_button->Enable(!path_.empty());
    open_button->Bind(wxEVT_BUTTON, &UnsupportedViewPanel::on_open_default, this);

    sizer->AddStretchSpacer();
    sizer->Add(title_text, 0, wxALIGN_CENTER | wxALL, 8);
    sizer->Add(detail_text, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, 16);
    sizer->Add(open_button, 0, wxALIGN_CENTER | wxALL, 12);
    sizer->AddStretchSpacer();

    SetSizer(sizer);
}

void UnsupportedViewPanel::on_open_default(wxCommandEvent&)
{
    if (path_.empty())
    {
        return;
    }
    // 既定のアプリ（OS の関連付け）で開く（要件12.2「外部アプリへ誘導」）。失敗は握り潰す
    // （導線として最善を尽くす・落とさない）。UTF-8 パスを正しくワイド化して渡す。
    wxLaunchDefaultApplication(u8(path_));
}

} // namespace pika::ui
