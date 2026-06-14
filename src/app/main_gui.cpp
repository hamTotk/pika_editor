// pika: GUI 本体（wxWidgets）。
// sprint 1 では「GUI exe ターゲットがリンク・起動できる」ことを満たす最小骨格のみ。
// MainFrame・ツリー・タブ・プレビュー等は後続スプリント（UI 章）で実装する。
#include <wx/wx.h>

namespace
{

class PikaApp : public wxApp
{
  public:
    bool OnInit() override
    {
        auto* frame = new wxFrame(nullptr, wxID_ANY, "pika", wxDefaultPosition, wxSize(800, 600));
        frame->Show(true);
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(PikaApp);
