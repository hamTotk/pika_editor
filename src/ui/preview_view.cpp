#include "ui/preview_view.h"

#include "controller/preview_builder.h"
#include "core/ipc/path_normalizer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <WebView2.h>

#include <wx/event.h>
#include <wx/ffile.h>
#include <wx/log.h>
#include <wx/sizer.h>
#include <wx/webview.h>

#include <string>

namespace pika::ui
{

namespace
{

// doc.pika 仮想ホスト上の予約パス。生成済みプレビュー HTML をページ本体として配信する固定名。
// ここへ実ナビゲート（LoadURL）することで、ページ URL=https://doc.pika/ となり本文中の相対
// 画像/リンクが doc.pika 基準で解決される（preview_builder.cpp:170 の設計前提）。
constexpr const char* kPreviewDocName = "__pika_preview__.html";

wxString u8(const std::string& s)
{
    return wxString::FromUTF8(s.c_str(), s.size());
}

// パーセントデコードを 1 回だけ行う（%2e%2e%2f
// 等のエンコード済みトラバーサルを正規化前に展開する）。 不正な %XX はそのまま残す（壊さず後段の
// under_root 検証へ委ねる）。
std::string url_decode_once(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f')
                {
                    return c - 'a' + 10;
                }
                if (c >= 'A' && c <= 'F')
                {
                    return c - 'A' + 10;
                }
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// 区切りを '\\' に統一する（接頭辞照合用）。
std::string to_backslash(std::string s)
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

// abs が root 配下（root 自身または root\... ）か。区切りを統一して接頭辞照合する。
bool under_root(const std::string& abs, const std::string& root)
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

// 拡張子（小文字・ドットなし）を取り出す。
std::string lower_ext(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size())
    {
        return {};
    }
    std::string ext = name.substr(dot + 1);
    for (char& c : ext)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

// 許可拡張子 → Content-Type（許可リスト方式。文書外/任意拡張子は配信しない＝C7）。
const char* content_type_for(const std::string& ext)
{
    if (ext == "css")
    {
        return "text/css; charset=utf-8";
    }
    if (ext == "js")
    {
        return "text/javascript; charset=utf-8";
    }
    if (ext == "png")
    {
        return "image/png";
    }
    if (ext == "jpg" || ext == "jpeg")
    {
        return "image/jpeg";
    }
    if (ext == "gif")
    {
        return "image/gif";
    }
    if (ext == "svg")
    {
        return "image/svg+xml";
    }
    if (ext == "woff2")
    {
        return "font/woff2";
    }
    return nullptr; // 許可外。
}

// 仮想ホスト URI から `https://<host>/` の後ろ（パス部）を取り出す。前提不一致なら空を返す。
bool strip_host_prefix(const std::string& uri, const char* host, std::string& rel_out)
{
    const std::string prefix = std::string("https://") + host + "/";
    if (uri.rfind(prefix, 0) != 0)
    {
        return false;
    }
    rel_out = uri.substr(prefix.size());
    // クエリ/フラグメントは捨てる（ファイル解決に使わない）。
    const std::size_t cut = rel_out.find_first_of("?#");
    if (cut != std::string::npos)
    {
        rel_out = rel_out.substr(0, cut);
    }
    return true;
}

// doc.pika のカスタムリソースハンドラ（design 6章 C7）。
// 要求パスを 1 回 URL デコード → root 基準で正規化 → 配下検証 → 許可拡張子のみ返す。
// `../`（および %2e%2e%2f 等のエンコード）での上位ディレクトリ抜けを遮断する。
class DocPikaHandler : public wxWebViewHandler
{
  public:
    DocPikaHandler(const std::string* doc_root, const std::string* preview_html)
        : wxWebViewHandler("doc.pika"), root_ptr_(doc_root), preview_html_ptr_(preview_html)
    {
        // scheme 名は virtual host と一致させる。"https" にすると wxWebViewEdge::LoadURL の
        // カスタムプロトコル・エミュレーションが "https://..." を誤って書き換え host を二重化する
        // （実機 F-002）。WebResourceRequested の照合・フィルタは virtual host で行われるため、
        // scheme 名を doc.pika にしても https://doc.pika/* の横取りは不変。
        SetVirtualHost("doc.pika");
    }

    void StartRequest(const wxWebViewHandlerRequest& request,
                      wxSharedPtr<wxWebViewHandlerResponse> response) override
    {
        std::string rel;
        // GetRawURI() を使う（実 URL）。GetURI() は wxWebViewEdge では "name:path" 形式で host を
        // 落とすため strip_host_prefix("https://host/") と噛み合わない（実機 F-002）。
        const std::string uri(request.GetRawURI().ToUTF8().data());
        if (!strip_host_prefix(uri, "doc.pika", rel))
        {
            response->FinishWithError();
            return;
        }
        // (1) 1 回だけ URL デコードして %2e%2e%2f 等を実体化する。
        rel = url_decode_once(rel);
        // (予約) 生成済みプレビュー HTML をページ本体として配信する（root に依らず先に判定）。
        // バイト列を保ったまま返し（UTF-8）、charset=utf-8 をヘッダで宣言する。文書外の実ファイル
        // ではなく pika 生成の信頼済み HTML。
        if (rel == kPreviewDocName)
        {
            const std::string html = preview_html_ptr_ ? *preview_html_ptr_ : std::string{};
            wxString content(html.data(), wxConvISO8859_1, html.size());
            response->SetContentType("text/html; charset=utf-8");
            response->Finish(content, wxConvISO8859_1);
            return;
        }
        const std::string root = root_ptr_ ? *root_ptr_ : std::string{};
        if (root.empty())
        {
            // 対象消滅時はマッピング解除＝すべて拒否する（文書外の露出面を作らない）。
            response->FinishWithError();
            return;
        }
        // (2) `..` を畳んで絶対化し、root の配下（接頭辞一致）に収まることを検証する（C7）。
        const std::string abs = core::ipc::normalize_to_absolute(rel, root);
        if (!under_root(abs, root))
        {
            response->FinishWithError();
            return;
        }
        // (3) 許可拡張子のみ配信する（許可リスト方式）。
        const char* ctype = content_type_for(lower_ext(abs));
        if (!ctype)
        {
            response->FinishWithError();
            return;
        }
        // 実ファイルを読んで返す。読めない場合はエラー（白紙ではなく欠落として扱う）。
        // 欠落サブリソース（壊れた相対画像など）は想定内の事象。wxFFile/wxLog が既定ログ先で
        // モーダルを出さないよう wxLogNull で抑止する（固まらない・内部パス非漏洩。F-006）。
        wxLogNull no_log;
        wxFFile file(u8(abs), "rb");
        if (!file.IsOpened())
        {
            response->FinishWithError();
            return;
        }
        wxString content;
        if (!file.ReadAll(&content, wxConvISO8859_1))
        {
            response->FinishWithError();
            return;
        }
        response->SetContentType(ctype);
        response->Finish(content, wxConvISO8859_1);
    }

  private:
    const std::string* root_ptr_;         // PreviewView::doc_root_ を指す（所有しない）
    const std::string* preview_html_ptr_; // PreviewView::preview_html_ を指す（所有しない）
};

// app.pika の同梱アセットハンドラ（design 6章 C6・must#4「仮想ホスト app.pika（同梱アセット）」）。
// CSP の script-src/style-src/img-src/font-src が https://app.pika を許可するため、preview.css 等の
// 同梱信頼アセットはここから配信する。配信元は exe 隣の assets/（asset_dir_）に限定する。
class AppPikaHandler : public wxWebViewHandler
{
  public:
    explicit AppPikaHandler(const std::string* asset_dir)
        : wxWebViewHandler("app.pika"), dir_ptr_(asset_dir)
    {
        // scheme 名を virtual host と一致させる（DocPikaHandler と同理由。実機 F-002）。
        SetVirtualHost("app.pika");
    }

    void StartRequest(const wxWebViewHandlerRequest& request,
                      wxSharedPtr<wxWebViewHandlerResponse> response) override
    {
        const std::string dir = dir_ptr_ ? *dir_ptr_ : std::string{};
        if (dir.empty())
        {
            response->FinishWithError();
            return;
        }
        std::string rel;
        // GetRawURI() を使う（実 URL）。GetURI() は wxWebViewEdge では "name:path" 形式で host を
        // 落とすため strip_host_prefix("https://host/") と噛み合わない（実機 F-002）。
        const std::string uri(request.GetRawURI().ToUTF8().data());
        if (!strip_host_prefix(uri, "app.pika", rel))
        {
            response->FinishWithError();
            return;
        }
        rel = url_decode_once(rel);
        // assets/ 配下に閉じる（doc.pika と同じ ../ 遮断・許可拡張子。文書フォルダではない）。
        const std::string abs = core::ipc::normalize_to_absolute(rel, dir);
        if (!under_root(abs, dir))
        {
            response->FinishWithError();
            return;
        }
        const char* ctype = content_type_for(lower_ext(abs));
        if (!ctype)
        {
            response->FinishWithError();
            return;
        }
        // 欠落アセットでも既定ログ先のモーダルを出さない（固まらない。F-006）。
        wxLogNull no_log;
        wxFFile file(u8(abs), "rb");
        if (!file.IsOpened())
        {
            response->FinishWithError();
            return;
        }
        wxString content;
        if (!file.ReadAll(&content, wxConvISO8859_1))
        {
            response->FinishWithError();
            return;
        }
        response->SetContentType(ctype);
        response->Finish(content, wxConvISO8859_1);
    }

  private:
    const std::string* dir_ptr_; // PreviewView::asset_dir_ を指す（所有しない）
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

void PreviewView::set_asset_dir(const std::string& asset_dir_abs)
{
    asset_dir_ = asset_dir_abs;
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

    // 仮想ホストのカスタムリソースハンドラ（design 6章 C6/C7）。
    //   doc.pika … 表示中文書の親フォルダ配下のみ（../ 遮断・許可拡張子）。
    //   app.pika … exe 隣 assets/ の同梱信頼アセット（preview.css 等）。
    web_->RegisterHandler(
        wxSharedPtr<wxWebViewHandler>(new DocPikaHandler(&doc_root_, &preview_html_)));
    web_->RegisterHandler(wxSharedPtr<wxWebViewHandler>(new AppPikaHandler(&asset_dir_)));

    // 表示専用に絞る（コンテキストメニュー・開発者ツールを無効化。design 6章）。
    web_->EnableContextMenu(false);
    web_->EnableAccessToDevTools(false);

    // ナビゲーションは全インターセプト（プレビュー内ページ遷移をさせない。design 6章）。
    web_->Bind(wxEVT_WEBVIEW_NAVIGATING, &PreviewView::on_navigating, this);
    web_->Bind(wxEVT_WEBVIEW_LOADED, &PreviewView::on_loaded, this);
}

void PreviewView::apply_script_enabled(bool enabled)
{
    // 二層目防御（design 6章 C5）: WebView2 の JS ランタイムを実際に有効/無効化する。
    // CSP（一層目・script-src https://app.pika）が文書由来 JS を止めるが、HTML プレビューでは
    // ランタイムごと無効化して前モードの残留・実行面を完全に断つ。失敗しても CSP が残る。
    if (!web_)
    {
        return;
    }
    auto* core_webview = static_cast<ICoreWebView2*>(web_->GetNativeBackend());
    if (!core_webview)
    {
        return;
    }
    ICoreWebView2Settings* settings = nullptr;
    if (FAILED(core_webview->get_Settings(&settings)) || !settings)
    {
        return;
    }
    settings->put_IsScriptEnabled(enabled ? TRUE : FALSE);
    settings->Release();
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
    // 直列化（design 6章 C5）: 前ナビが完了していなければ最新要求だけを保留し、on_loaded で流す。
    // ナビゲート中に JS 設定を変えると前モード設定が次ロードへ残留し得るため、必ず完了を待つ。
    if (nav_in_flight_)
    {
        pending_html_ = full_html;
        pending_kind_ = kind;
        has_pending_ = true;
        return;
    }
    // JS 有効/無効を内容種別で切替える（Markdown/差分=有効・HTML=無効。design 6章 C5）。
    // 必ずナビゲート前に設定し、ナビゲート完了（on_loaded）まで次のロードを始めない。
    js_enabled_ = (kind == controller::PreviewKind::Markdown);
    apply_script_enabled(js_enabled_);
    nav_in_flight_ = true;
    // 生成 HTML は doc.pika の予約パスから本体配信し、ページ URL=https://doc.pika/ へ実ナビゲート
    // する（preview_builder.cpp:170 の前提）。これで本文中の相対画像/リンクが doc.pika 基準で解決
    // される。wxWebViewEdge::SetPage は baseUrl を無視し NavigateToString を使うため相対解決できず
    // 初回ロードも Veto されていた（実機 F-002）。CSP が JS の実効境界を担う（C6 二重防御）。
    // 同一 URL でも内容差し替えで再ロードさせるため ?g=世代 を付す（クエリはハンドラ側で破棄）。
    preview_html_ = full_html;
    ++nav_gen_;
    const std::string url =
        std::string("https://doc.pika/") + kPreviewDocName + "?g=" + std::to_string(nav_gen_);
    web_->LoadURL(u8(url));
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

std::uint64_t PreviewView::request_occupy(const controller::OccupancyKey& key)
{
    // 要求時点で占有して stamp を確定する（ワーカー投入前。design 5.5 手順3）。
    occupancy_.occupy(key);
    return occupancy_.generation();
}

void PreviewView::apply_document(std::uint64_t stamp, const controller::OccupancyKey& key,
                                 const std::string& full_html, controller::PreviewKind js_kind)
{
    // ワーカー完了後の適用（UI スレッド）。要求時の stamp/key がまだ最新のときだけ反映する。
    // 別タブ/別モード/別差分へ切替済みなら破棄して最新リクエストを待つ（中間状態を描かない）。
    if (!occupancy_.is_current(stamp, key))
    {
        return;
    }
    navigate(full_html, js_kind);
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
    // 生成プレビュー本体（doc.pika の予約パス）と WebView2 内部の初期ロード（空 URL 生成時の
    // about:blank 等）は通す。それ以外のトップレベル遷移＝プレビュー内のページ遷移（リンク
    // クリック等）は許さず pika 側へ振り分ける（design 6章）。サブリソース（CSS/画像）は
    // ハンドラ経由で NAVIGATING を起こさないため、ここに来るのはトップレベル遷移のみ。
    const std::string url(evt.GetURL().ToUTF8().data());
    const std::string preview_doc = std::string("https://doc.pika/") + kPreviewDocName;
    const bool is_preview_doc = url.rfind(preview_doc, 0) == 0;
    const bool is_internal_blank =
        url == "about:blank" || url.rfind("about:", 0) == 0 || url.rfind("data:", 0) == 0;
    if (is_preview_doc || is_internal_blank)
    {
        evt.Skip(); // 内部ロードは通す
        return;
    }
    evt.Veto(); // ユーザー操作によるトップレベル遷移はキャンセル
    if (on_navigate_)
    {
        on_navigate_(url); // 相対 .md/.html はタブ・他は既定ブラウザ（MainFrame が判定）
    }
}

void PreviewView::on_loaded(wxWebViewEvent& evt)
{
    // NavigationCompleted 相当（直列化の解除点・スクロール復元の照合点。design 5.5 手順3）。
    // 完了したので in-flight を解く。保留があれば「最新」を 1 件だけ流す（中間状態を描かない）。
    nav_in_flight_ = false;
    if (has_pending_)
    {
        has_pending_ = false;
        const std::string html = std::move(pending_html_);
        const controller::PreviewKind kind = pending_kind_;
        pending_html_.clear();
        navigate(html, kind); // ここで JS 設定はこの kind に対して再適用される（直列継続）。
    }
    // スクロール復元/占有世代の再照合の実描画は系統C で検証する。
    evt.Skip();
}

} // namespace pika::ui
