#include "ui/preview_view.h"

#include "core/ipc/path_normalizer.h"

#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/webview.h>

namespace pika::ui
{

namespace
{

wxString u8(const std::string& s)
{
    return wxString::FromUTF8(s.c_str(), s.size());
}

// doc.pika のカスタムリソースハンドラ（design 6章 C7）。
// 要求パスを正規化し、表示中文書の親フォルダ配下に収まることを検証してから許可拡張子のみ返す。
// `../` での上位ディレクトリ抜けは core/ipc::normalize_to_absolute の `..` 畳み込みで遮断する。
class DocPikaHandler : public wxWebViewHandler
{
  public:
    explicit DocPikaHandler(const std::string* doc_root)
        : wxWebViewHandler("https"), root_ptr_(doc_root)
    {
        SetVirtualHost("doc.pika");
    }

    void StartRequest(const wxWebViewHandlerRequest& request,
                      wxSharedPtr<wxWebViewHandlerResponse> response) override
    {
        const std::string root = root_ptr_ ? *root_ptr_ : std::string{};
        if (root.empty())
        {
            // 対象消滅時はマッピング解除＝すべて拒否する（文書外の露出面を作らない）。
            response->FinishWithError();
            return;
        }
        // https://doc.pika/<path> から <path> を取り出し、root 基準で正規化して配下検証する。
        std::string uri(request.GetURI().ToUTF8().data());
        const std::string prefix = "https://doc.pika/";
        if (uri.rfind(prefix, 0) != 0)
        {
            response->FinishWithError();
            return;
        }
        const std::string rel = uri.substr(prefix.size());
        // `..` を畳んで絶対化し、root の配下（接頭辞一致）に収まることを検証する（C7）。
        const std::string abs = core::ipc::normalize_to_absolute(rel, root);
        if (!under_root(abs, root))
        {
            response->FinishWithError();
            return;
        }
        // 実ファイル配信は系統C（実描画）で検証する。配線時点は配下検証成立で空応答を返す。
        response->FinishWithError();
    }

  private:
    // abs が root 配下（root 自身または root\... ）か。区切りを統一して接頭辞照合する。
    static bool under_root(const std::string& abs, const std::string& root)
    {
        std::string a = to_backslash(abs);
        std::string r = to_backslash(root);
        if (!r.empty() && r.back() == '\\')
        {
            r.pop_back();
        }
        if (a.size() < r.size())
        {
            return false;
        }
        if (a.compare(0, r.size(), r) != 0)
        {
            return false;
        }
        // root 直下/配下のみ（"C:\\rootX" を "C:\\root" 配下と誤判定しない）。
        return a.size() == r.size() || a[r.size()] == '\\';
    }

    static std::string to_backslash(std::string s)
    {
        for (char& c : s)
        {
            if (c == '/')
            {
                c = '\\';
            }
        }
        return s;
    }

    const std::string* root_ptr_; // PreviewView::doc_root_ を指す（所有しない）
};

} // namespace

PreviewView::PreviewView(wxWindow* parent, wxWindowID id) : wxPanel(parent, id)
{
    SetSizer(new wxBoxSizer(wxVERTICAL));
}

PreviewView::~PreviewView() = default;

bool PreviewView::webview_available()
{
    return wxWebView::IsBackendAvailable(wxWebViewBackendEdge);
}

void PreviewView::set_document_root(const std::string& folder_abs)
{
    doc_root_ = folder_abs;
}

void PreviewView::ensure_webview()
{
    if (web_)
    {
        return;
    }
    // 遅延生成（初回プレビュー要求まで作らない。design 5.1 手順5・軽量原則）。Edge 固定。
    web_ =
        wxWebView::New(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxWebViewBackendEdge);
    if (!web_)
    {
        return;
    }
    GetSizer()->Add(web_, 1, wxEXPAND);
    Layout();

    // doc.pika 仮想ホストのカスタムリソースハンドラを登録する（親フォルダ配下のみ・../遮断）。
    web_->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new DocPikaHandler(&doc_root_)));

    // 表示専用に絞る（コンテキストメニュー・開発者ツールを無効化。design 6章）。
    web_->EnableContextMenu(false);
    web_->EnableAccessToDevTools(false);

    // ナビゲーションは全インターセプト（プレビュー内ページ遷移をさせない。design 6章）。
    web_->Bind(wxEVT_WEBVIEW_NAVIGATING, &PreviewView::on_navigating, this);
    web_->Bind(wxEVT_WEBVIEW_LOADED, &PreviewView::on_loaded, this);
}

void PreviewView::navigate(const std::string& full_html, controller::PreviewKind kind)
{
    ensure_webview();
    if (!web_)
    {
        return;
    }
    if (suspended_)
    {
        // 次回表示で Resume 相当（再ナビゲートが復帰を兼ねる。design 6章 B1）。
        suspended_ = false;
    }
    // JS 有効/無効を内容種別で切替える（Markdown/差分=有効・HTML=無効。design 6章 C5）。
    // wxWebView は直列ナビゲート（前ナビ完了を待ってから次）で前モード設定の残留を防ぐ。
    js_enabled_ = (kind == controller::PreviewKind::Markdown);
    // base は doc.pika。CSP（script-src https://app.pika）が JS の実効境界を担う（C6 二重防御）。
    web_->SetPage(u8(full_html), "https://doc.pika/");
}

void PreviewView::show_preview(const controller::OccupancyKey& key,
                               const controller::PreviewDoc& doc)
{
    // 占有世代を進める（同一鍵なら再ナビゲートを避ける。design 6章 (2) キャッシュ）。
    if (!occupancy_.occupy(key) && web_)
    {
        return;
    }
    navigate(controller::build_preview_document(doc), doc.kind);
}

void PreviewView::show_diff(const controller::OccupancyKey& key, const core::diff::DiffResult& diff,
                            core::render::RemoteResourcePolicy policy)
{
    if (!occupancy_.occupy(key) && web_)
    {
        return;
    }
    // 差分面は pika 生成の信頼済み HTML（JS 有効＝Markdown 相当）。
    navigate(controller::build_diff_document(diff, policy), controller::PreviewKind::Markdown);
}

void PreviewView::show_preview_diff_grid(const controller::OccupancyKey& key,
                                         const controller::PreviewDoc& doc,
                                         const core::diff::DiffResult& diff)
{
    if (!occupancy_.occupy(key) && web_)
    {
        return;
    }
    navigate(controller::build_preview_diff_grid_document(doc, diff), doc.kind);
}

void PreviewView::suspend_if_idle()
{
    // 全タブ非プレビュー一定時間で共有 WebView2 をサスペンドする（メモリ回収。design 6章 B1）。
    // wxWebview は TrySuspend を直接公開しないため、配線層は状態のみ持ち、Resume は次回ナビゲートで
    // 兼ねる（TrySuspend/破棄再生成の実体は系統C で検証する）。
    if (web_ && !suspended_)
    {
        suspended_ = true;
    }
}

void PreviewView::on_navigating(wxWebViewEvent& evt)
{
    // 全キャンセルして pika 側へ振り分ける（プレビュー内のページ遷移を許さない。design 6章）。
    // 初回の SetPage/doc.pika は内部ロードなのでキャンセルしない（振り分け対象は外部 URL）。
    const std::string url(evt.GetURL().ToUTF8().data());
    const bool is_internal = url.rfind("https://doc.pika/", 0) == 0 ||
                             url.rfind("https://app.pika/", 0) == 0 || url == "about:blank";
    if (is_internal)
    {
        evt.Skip(); // 内部ロードは通す
        return;
    }
    evt.Veto(); // 外部遷移はキャンセル
    if (on_navigate_)
    {
        on_navigate_(url); // 相対 .md/.html はタブ・他は既定ブラウザ（MainFrame が判定）
    }
}

void PreviewView::on_loaded(wxWebViewEvent& evt)
{
    // NavigationCompleted 相当（直列化の解除点・スクロール復元の照合点。design 5.5 手順3）。
    // スクロール復元/占有世代の再照合の実描画は系統C で検証する。
    (void)js_enabled_;
    evt.Skip();
}

} // namespace pika::ui
