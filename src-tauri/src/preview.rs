//! プレビュー（権限ゼロ別WebView＋custom protocol 直配信）の配線（要件6・design doc 6章/15章-3）。
//!
//! 役割（design doc 3章「薄い境界」）:
//! - サニタイズ（comrak→ammonia）・CSP 組立・パス封じ込め・暴走ガードは **すべて pika-core::render**
//!   （cargo test 済み）に委ね、ここは「サニタイズ済みレスポンスの保持」と「custom protocol 直配信」
//!   「権限ゼロ別WebView の生成」に徹する。
//! - **プレビュー HTML は invoke で返さない**（IPC 予算＋オリジン分離）。custom protocol(`pika-preview://`)
//!   が Rust から別WebView へ直接配信し、HTML を JS のメインワールドに通さない（design doc 6章）。
//! - 別WebView は **capability を一切付与しない**（capability ファイルが `preview` ラベルを含まない＝権限ゼロ）。
//!   これにより未信頼文書 WebView から invoke/__TAURI_INTERNALS__ 経由の任意 command が到達不能になる。
//!   到達不能の実証は系統C（Windows 実機 Release・design doc 15章-3）。

use include_dir::{include_dir, Dir};
use pika_core::render::{
    check_image_bytes, check_svg_bytes, confine_under, join_under, prepare_html_preview,
    prepare_markdown_preview, resolve_local_ref, rewrite_local_image_refs, wrap_preview_document,
    ExternalResourceAllow, GuardDecision, LocalRefDecision, PreviewFlavor, PreviewResponse,
    DEFAULT_IMAGE_MAX_PIXELS, DEFAULT_SVG_MAX_ELEMENTS, DEFAULT_SVG_MAX_PIXELS,
};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;
use tauri::http::{header, Response, StatusCode};
use tauri::{LogicalPosition, LogicalSize, Manager, State, UriSchemeContext};

/// プレビュー別WebView のラベル（capability マップに**含めない**＝権限ゼロ・design doc 6章/9章）。
pub const PREVIEW_WEBVIEW_LABEL: &str = "preview";

/// メインウィンドウのラベル（tauri.conf.json と一致）。子WebView はこの窓のクライアント領域に重ねる。
const MAIN_WINDOW_LABEL: &str = "main";

/// プレビュー custom protocol のスキーム名。
pub const PREVIEW_SCHEME: &str = "pika-preview";

/// 同梱ベンダーアセット（Mermaid/KaTeX/highlight・KaTeX フォント・約3.84MiB）を exe へ埋め込む（Stage ②）。
///
/// 自己完結・ポータブル配布のため実行時 FS 依存にしない（design doc 1章「軽い/ワークスペースを汚さない」）。
/// 仮想 FS（`include_dir`）からビルド時に畳み込み、`/assets/<相対パス>` で別WebView へ直配信する。
/// アセットは custom protocol 経由でのみ別WebView に閉じ、メイン WebView の JS ワールドを経由しない。
static PREVIEW_ASSETS: Dir<'_> = include_dir!("$CARGO_MANIFEST_DIR/../assets/vendor");

/// プレビュー要求の系統指定（frontend から invoke で渡す＝要件6.1/6.3）。
#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PreviewMode {
    /// 系統A: Markdown を comrak→ammonia でレンダリング（信頼 JS を別WebView へ nonce 注入）。
    Markdown,
    /// 系統B: ユーザー文書 HTML（JS 完全無効・インライン CSS 尊重）。
    Html,
}

/// `prepare_preview` の戻り（frontend は別WebView の URL を切り替えるだけ＝design doc 6章）。
#[derive(Debug, Clone, Serialize)]
pub struct PreparedPreview {
    /// 別WebView へナビゲートする URL（`pika-preview://localhost/doc/<gen>`）。
    pub url: String,
    /// 占有世代（タブ/モード/差分の切替直列化＝design doc 6章/ui-design 8章）。
    /// frontend は最新世代の load 完了のみ採用し前モード残留を防ぐ。
    pub generation: u64,
    /// 系統A の信頼 JS 注入に使う nonce（系統B では空）。CSP の `script-src 'nonce-<rnd>'` と一致する。
    pub nonce: String,
    /// 系統（"markdown" | "html"）。frontend が信頼 JS 注入の要否を判断する。
    pub flavor: String,
    /// 系統B（HTML）の危険検知（要件6.3・通知バー導線）。系統A では全 false。
    ///
    /// **content を 2 回 invoke しない**ため prepare_preview の戻りに同梱する（IPC 二重転送回避＝
    /// performance 指摘）。系統A（Markdown）は danger 検知の対象外で常に全 false。
    pub hazards: HtmlHazards,
}

/// プレビューサービス（managed state）。サニタイズ済みレスポンスを世代キーで保持する。
///
/// custom protocol ハンドラはこのストアから「現在配信すべきサニタイズ済み HTML＋CSP」を引く。
/// 別WebView はこのストアにしかアクセスできず、Tauri API には到達できない（権限ゼロ）。
pub struct PreviewService {
    inner: Mutex<PreviewInner>,
}

#[derive(Default)]
struct PreviewInner {
    /// 占有世代カウンタ（タブ/モード/差分切替で単調増加）。
    generation: u64,
    /// 世代 → サニタイズ済みレスポンス（直近のみ保持・古い世代は破棄）。
    documents: HashMap<u64, PreparedDoc>,
}

/// 1 プレビュー文書の保持データ（custom protocol が配信する素材）。
struct PreparedDoc {
    /// サニタイズ済み HTML＋CSP＋nonce（pika-core::render の成果）。
    /// オプトイン外部許可は CSP（`response.csp`）に既に織り込み済み（design doc 6章）。
    response: PreviewResponse,
    /// ローカル相対参照（画像/CSS）を解決する基準ディレクトリ（canonicalize 済み）。
    /// `None` は基準ディレクトリ不明（FS 上に無い等）＝ローカル参照は配信しない。
    base_dir: Option<PathBuf>,
}

impl Default for PreviewService {
    fn default() -> Self {
        Self {
            inner: Mutex::new(PreviewInner::default()),
        }
    }
}

impl PreviewService {
    pub fn new() -> Self {
        Self::default()
    }
}

/// 文書をプレビュー用に準備し、別WebView へナビゲートする URL を返す（要件6・design doc 6章）。
///
/// サニタイズ（comrak→ammonia）・CSP 組立は pika-core::render で行い、結果を世代キーで保持する。
/// **HTML 本体は invoke の戻り値に乗せない**（URL のみ返す）。別WebView が custom protocol で取得する。
///
/// - `path`: 文書のパス（ローカル相対参照の基準ディレクトリ算出に使う）。
/// - `mode`: 系統A（Markdown）/ 系統B（HTML）。
/// - `content`: 文書内容（編集バッファ or ディスク内容）。invoke の戻りに HTML を乗せないため入力のみ。
/// - `allow_external`: オプトイン外部許可ホスト（既定は空＝外部遮断）。
#[tauri::command]
pub fn prepare_preview(
    path: String,
    mode: PreviewMode,
    content: String,
    allow_external: Option<Vec<String>>,
    preview: State<'_, PreviewService>,
) -> Result<PreparedPreview, String> {
    let allow = ExternalResourceAllow {
        hosts: allow_external.unwrap_or_default(),
    };

    // サニタイズ＋CSP（pika-core::render）。HTML 本体は以後ストアにのみ置き、invoke で返さない。
    // 系統B の危険検知（要件6.3）も同じ content からこの 1 回で済ませる（IPC 二重転送回避）。
    let mut response = match mode {
        PreviewMode::Markdown => prepare_markdown_preview(&content, &allow),
        PreviewMode::Html => prepare_html_preview(&content, &allow),
    };
    let hazards = match mode {
        // 系統A（Markdown）は文書 JS 検知の対象外（comrak/ammonia でサニタイズ済み）。
        PreviewMode::Markdown => HtmlHazards::default(),
        // 系統B（HTML）は素の文書から危険検知して通知バー導線に使う（要件6.3）。
        PreviewMode::Html => scan_hazards(&content),
    };

    // ローカル相対参照の基準ディレクトリ（canonicalize して prefix 検証の基準にする＝design doc 6章）。
    let base_dir = PathBuf::from(&path)
        .parent()
        .and_then(|p| std::fs::canonicalize(p).ok());

    let mut inner = preview.inner.lock().map_err(|_| "preview ロック失敗")?;
    inner.generation += 1;
    let generation = inner.generation;

    // 文書本文（サニタイズ済み body）の相対 <img src> を配信ルート `/local/<gen>/` へ書き換える。
    // 背景: 別WebView の文書 URL は `…/doc/<gen>` のため、相対 `img/x.png` はブラウザにより
    // `/doc/img/x.png` に解決され、ローカル配信ルート `/local/<gen>/<相対パス>` に届かず 404 になる。
    // ここで配信ルートへ前置する（前置のみ・`..`/絶対化はしない）。封じ込め検証は配信時の
    // local_resource_response（resolve_local_ref＋canonicalize+prefix）が従来どおり担う。
    // 系統A（Markdown）/系統B（HTML）双方に適用する（両系統とも相対画像を持ち得る）。書き換えは
    // wrap_preview_document でラップする前の素の body に対して行う（/assets/・/local/ は絶対ルートで
    // 相対解決の影響を受けない）。
    let local_prefix = format!("/local/{generation}/");
    response.body = rewrite_local_image_refs(&response.body, &local_prefix);

    let nonce = response.nonce.clone();
    let flavor = match response.flavor {
        PreviewFlavor::MarkdownTrustedJs => "markdown",
        PreviewFlavor::HtmlNoJs => "html",
    }
    .to_string();

    // 直近 1 世代のみ保持（メモリ節約＋前モード残留防止）。古い文書素材は破棄する。
    inner.documents.clear();
    inner
        .documents
        .insert(generation, PreparedDoc { response, base_dir });

    // Windows/Android では custom protocol は http://<scheme>.localhost/ に解決される（Tauri 仕様）。
    // frontend はこの URL を別WebView の src へ設定する（HTML は JS を一切経由しない）。
    let url = format!("http://{PREVIEW_SCHEME}.localhost/doc/{generation}");
    Ok(PreparedPreview {
        url,
        generation,
        nonce,
        flavor,
        hazards,
    })
}

/// メインウィンドウへ **権限ゼロのプレビュー別WebView（子WebView オーバーレイ）** を生成する（design doc 6章/9章）。
///
/// `setup()` から一度だけ呼ぶ。初期は極小サイズ・画面外配置・`about:blank` の hidden 状態にしておき、
/// frontend が `show_preview` を呼んだ時に矩形へ配置・表示・ナビゲートする（中心体験の核）。
///
/// 権限ゼロの担保（design doc 6章・最重要）:
/// - **capability ファイルに `preview` ラベルを含めない**（`capabilities/main.json` は `["main"]` のみ）。
/// - 別WebView は `pika-preview.localhost`（**remote オリジン**）へナビゲートするため、Tauri の ACL は
///   remote オリジンからの app command を拒否する（`invoke`/`__TAURI_INTERNALS__` 経由の到達不能）。
/// - 念のため `browser_extensions_enabled(false)` で拡張機能経由の注入面も塞ぐ。
///
/// マルチ WebView（`Window::add_child`）は Tauri の `unstable` feature を要する（Cargo.toml で有効化）。
pub fn create_preview_webview(app: &tauri::AppHandle) -> tauri::Result<()> {
    // 既に生成済みなら何もしない（多重生成防止）。
    if app.get_webview(PREVIEW_WEBVIEW_LABEL).is_some() {
        return Ok(());
    }
    let Some(main) = app.get_window(MAIN_WINDOW_LABEL) else {
        // メイン窓が無い異常系。プレビュー無しでもアプリは動く（中心体験の編集は継続）。
        return Ok(());
    };

    // about:blank の空ページで生成する。HTML 本体は custom protocol が show_preview 時に直配信する。
    let builder = tauri::webview::WebviewBuilder::new(
        PREVIEW_WEBVIEW_LABEL,
        tauri::WebviewUrl::External("about:blank".parse().expect("about:blank は妥当な URL")),
    )
    // 拡張機能経由の注入面を塞ぐ（権限ゼロの多層防御）。
    .browser_extensions_enabled(false)
    // プレビューは文書のドラッグ＆ドロップを受けない（アプリシェル側の DnD と混線させない）。
    .disable_drag_drop_handler();

    // 初期は極小・画面外・hidden（show_preview で矩形へ配置・表示する）。
    let webview = main.add_child(
        builder,
        LogicalPosition::new(0.0, 0.0),
        LogicalSize::new(1.0, 1.0),
    )?;
    let _ = webview.hide();
    Ok(())
}

/// プレビュー別WebView を指定矩形（`#preview-host` の DOM 矩形・CSS ピクセル）へ配置・表示し `url` へナビゲートする。
///
/// 座標はメインウィンドウのクライアント領域基準の論理座標（CSS ピクセル）。DPI は Tauri の論理座標系へ委ねる。
/// frontend は表示モードが preview/split のときのみ呼ぶ（占有解決は resolveOccupancy）。
#[tauri::command]
pub fn show_preview(
    app: tauri::AppHandle,
    x: f64,
    y: f64,
    w: f64,
    h: f64,
    url: String,
) -> Result<(), String> {
    let webview = app
        .get_webview(PREVIEW_WEBVIEW_LABEL)
        .ok_or("プレビュー別WebView 未生成")?;
    apply_bounds(&webview, x, y, w, h)?;
    let parsed = url.parse().map_err(|_| "プレビュー URL が不正")?;
    webview.navigate(parsed).map_err(|e| e.to_string())?;
    webview.show().map_err(|e| e.to_string())?;
    Ok(())
}

/// プレビュー別WebView を隠す（占有がソース等プレビュー非表示のとき）。
///
/// 再表示できる方式（hide）にする。次の show_preview で再ナビゲートするため about:blank へは戻さない
/// （戻すと一瞬の空白が見えるだけで利点が無い・hide で十分）。
#[tauri::command]
pub fn hide_preview(app: tauri::AppHandle) -> Result<(), String> {
    // 別WebView 未生成（極端な異常系）でも frontend 側のトグルは成功扱いにする（編集継続を阻害しない）。
    if let Some(webview) = app.get_webview(PREVIEW_WEBVIEW_LABEL) {
        webview.hide().map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// プレビュー別WebView の位置・サイズのみ更新する（ペイン/ウィンドウ resize への領域追従）。
///
/// frontend の `ResizeObserver`/window resize から `#preview-host` の矩形を渡す。表示中のみ呼ぶ
/// （非表示中は無害だが frontend 側で抑止する）。
#[tauri::command]
pub fn set_preview_bounds(
    app: tauri::AppHandle,
    x: f64,
    y: f64,
    w: f64,
    h: f64,
) -> Result<(), String> {
    if let Some(webview) = app.get_webview(PREVIEW_WEBVIEW_LABEL) {
        apply_bounds(&webview, x, y, w, h)?;
    }
    Ok(())
}

/// 子WebView の位置・サイズを論理座標で適用する（show_preview/set_preview_bounds の共通処理）。
///
/// サイズが 0 以下になり得る（領域未確定/縮退）ため最小 1px に丸める（WebView2 が 0 サイズを嫌うため）。
fn apply_bounds(webview: &tauri::Webview, x: f64, y: f64, w: f64, h: f64) -> Result<(), String> {
    let w = w.max(1.0);
    let h = h.max(1.0);
    webview
        .set_position(LogicalPosition::new(x, y))
        .map_err(|e| e.to_string())?;
    webview
        .set_size(LogicalSize::new(w, h))
        .map_err(|e| e.to_string())?;
    Ok(())
}

/// `pika-preview://`（実体 `http://pika-preview.localhost/`）の custom protocol ハンドラ。
///
/// ルーティング:
/// - `/doc/<gen>` — サニタイズ済み HTML 本体を CSP ヘッダ付きで配信（design doc 6章）。
/// - `/local/<gen>/<相対パス>` — 文書フォルダ配下のローカル参照（画像/CSS）を封じ込め検証して配信。
///
/// CSP は **必ずレスポンスヘッダで返す**（文書内 `<meta>` には依存しない）。パス封じ込め・機密拒否は
/// pika-core::render::path（cargo test 済み）に委ねる。
pub fn handle_preview_request(
    ctx: UriSchemeContext<'_, tauri::Wry>,
    request: tauri::http::Request<Vec<u8>>,
) -> Response<Vec<u8>> {
    let app = ctx.app_handle();
    let Some(preview) = app.try_state::<PreviewService>() else {
        return error_response(StatusCode::INTERNAL_SERVER_ERROR, "preview state 未登録");
    };

    let path = request.uri().path().to_string();
    let inner = match preview.inner.lock() {
        Ok(g) => g,
        Err(_) => return error_response(StatusCode::INTERNAL_SERVER_ERROR, "preview ロック失敗"),
    };

    // /doc/<gen> — サニタイズ済み本体配信。
    if let Some(rest) = path.strip_prefix("/doc/") {
        let Ok(generation) = rest.trim_end_matches('/').parse::<u64>() else {
            return error_response(StatusCode::BAD_REQUEST, "不正な世代");
        };
        let Some(doc) = inner.documents.get(&generation) else {
            return error_response(StatusCode::NOT_FOUND, "プレビュー素材なし（世代失効）");
        };
        return document_response(doc);
    }

    // /assets/<相対パス> — exe 埋め込みの同梱ベンダーアセット（Mermaid/KaTeX/highlight・フォント・CSS）。
    // 世代に依存しない静的配信。ロックは不要だが、ここではすでに取得済みなので保持したまま返す。
    if let Some(rest) = path.strip_prefix("/assets/") {
        // ロックを早めに解放（静的配信は PreviewService を読まない）。
        drop(inner);
        return asset_response(rest);
    }

    // /local/<gen>/<相対パス> — ローカル参照（画像/CSS）の封じ込め配信。
    if let Some(rest) = path.strip_prefix("/local/") {
        let mut parts = rest.splitn(2, '/');
        let gen_str = parts.next().unwrap_or("");
        let reference = parts.next().unwrap_or("");
        let Ok(generation) = gen_str.parse::<u64>() else {
            return error_response(StatusCode::BAD_REQUEST, "不正な世代");
        };
        let Some(doc) = inner.documents.get(&generation) else {
            return error_response(StatusCode::NOT_FOUND, "プレビュー素材なし（世代失効）");
        };
        return local_resource_response(doc, reference);
    }

    error_response(StatusCode::NOT_FOUND, "不明な経路")
}

/// サニタイズ済み HTML 本体を CSP ヘッダ付きで返す（design doc 6章）。
///
/// pika-core のサニタイズ済み body は `<head>`/`<!DOCTYPE>` を持たない**フラグメント**なので、
/// 別WebView へ配信する前に [`wrap_preview_document`]（pika-core・cargo test 済み）で
/// 完全 HTML 文書（charset utf-8・最小 base CSS）にラップする。**body は一字一句改変しない**。
/// CSP は引き続きレスポンスヘッダで強制し、文書内 `<meta>` には依存しない（design doc 6章）。
fn document_response(doc: &PreparedDoc) -> Response<Vec<u8>> {
    let html = wrap_preview_document(&doc.response.body, &doc.response.nonce, doc.response.flavor);
    // CSP は pika-core が組んだ値（既定外部遮断・nonce・オプトイン緩和は img/font のみ）。
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "text/html; charset=utf-8")
        .header(header::CONTENT_SECURITY_POLICY, &doc.response.csp)
        // クリックジャッキング/MIME スニッフィング/参照漏れの追加防御。
        .header("X-Content-Type-Options", "nosniff")
        .header(header::REFERRER_POLICY, "no-referrer")
        .body(html.into_bytes())
        .unwrap_or_else(|_| error_response(StatusCode::INTERNAL_SERVER_ERROR, "応答組立失敗"))
}

/// ローカル参照（画像/CSS）を封じ込め検証して返す（要件6.2/6.3/9.1・design doc 6章）。
///
/// pika-core::render::path で `../`/絶対パス/機密ファイルを拒否し、canonicalize+prefix 検証で
/// シンボリックリンク脱出を弾く。配信できない参照は 403/404 を返す（frontend がプレースホルダ表示）。
fn local_resource_response(doc: &PreparedDoc, reference: &str) -> Response<Vec<u8>> {
    let Some(base) = &doc.base_dir else {
        return error_response(StatusCode::NOT_FOUND, "基準ディレクトリ不明");
    };

    // パス封じ込め判定（FS 非依存の純粋ロジック＝cargo test 済み）。
    let relative = match resolve_local_ref(reference) {
        LocalRefDecision::Relative(rel) => rel,
        LocalRefDecision::Reject(_) => {
            // 機密/脱出/絶対は配信拒否（403）。frontend が壊れた参照のプレースホルダにする。
            return error_response(StatusCode::FORBIDDEN, "ローカル参照を拒否");
        }
    };

    // 基準へ結合し canonicalize → prefix 検証（シンボリックリンク脱出をここで弾く＝design doc 6章）。
    let joined = join_under(base, &relative);
    let Ok(resolved) = std::fs::canonicalize(&joined) else {
        return error_response(StatusCode::NOT_FOUND, "参照先なし");
    };
    if !confine_under(base, &resolved) {
        // canonicalize がリンクを実体へ展開した結果が基準外＝脱出。配信拒否。
        return error_response(StatusCode::FORBIDDEN, "基準ディレクトリ外への脱出");
    }
    // 多層防御: 非機密名のシンボリックリンクが基準内の機密ファイルを指すケースを、
    // 解決後の実体名でも機密判定して弾く（要件9.1「機密は custom protocol からも配信拒否」）。
    if pika_core::snapshot::policy::is_sensitive(&resolved.to_string_lossy()) {
        return error_response(StatusCode::FORBIDDEN, "機密ファイルの配信拒否");
    }

    let Ok(bytes) = std::fs::read(&resolved) else {
        return error_response(StatusCode::NOT_FOUND, "読み取り失敗");
    };
    let content_type = guess_content_type(&resolved);

    // 暴走ガード（要件2.2・design doc 6章「WebView 任せにしない」）。巨大画像/SVG はデコード前に
    // ヘッダ寸法/要素数を pika-core::render で計測し、超過は配信せず 413 で弾く（UI が固まらない）。
    // frontend は 413 をプレースホルダ＋通知導線（既定のアプリで開く）にする。
    if let GuardDecision::Block(_) = guard_local_resource(content_type, &bytes) {
        return error_response(
            StatusCode::PAYLOAD_TOO_LARGE,
            "暴走ガード: 上限超過のため配信拒否",
        );
    }

    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, content_type)
        .header("X-Content-Type-Options", "nosniff")
        .body(bytes)
        .unwrap_or_else(|_| error_response(StatusCode::INTERNAL_SERVER_ERROR, "応答組立失敗"))
}

/// ローカルリソース配信前の暴走ガード（要件2.2）。Content-Type で画像/SVG を判別し pika-core で判定する。
///
/// 判定ロジック本体（ヘッダ寸法読取・SVG 要素数計数）は pika-core::render::guard（cargo test 済み）。
/// 画像/SVG 以外（CSS/フォント）は対象外で常に Allow（巨大化してもデコード爆発しないため）。
fn guard_local_resource(content_type: &str, bytes: &[u8]) -> GuardDecision {
    if content_type == "image/svg+xml" {
        check_svg_bytes(bytes, DEFAULT_SVG_MAX_PIXELS, DEFAULT_SVG_MAX_ELEMENTS)
    } else if content_type.starts_with("image/") {
        check_image_bytes(bytes, DEFAULT_IMAGE_MAX_PIXELS)
    } else {
        GuardDecision::Allow
    }
}

/// 拡張子から最小限の Content-Type を推定する（ローカル画像/CSS の配信用）。
fn guess_content_type(path: &std::path::Path) -> &'static str {
    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    match ext.as_str() {
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "bmp" => "image/bmp",
        "ico" => "image/x-icon",
        "svg" => "image/svg+xml",
        "css" => "text/css; charset=utf-8",
        // 同梱ベンダー JS（/assets/*.min.js）の配信に必要（Stage ② で追加）。
        "js" | "mjs" => "text/javascript; charset=utf-8",
        "woff" => "font/woff",
        "woff2" => "font/woff2",
        "ttf" => "font/ttf",
        _ => "application/octet-stream",
    }
}

/// exe 埋め込みの同梱ベンダーアセットを `/assets/<相対パス>` から配信する（Stage ②・design doc 6章）。
///
/// セキュリティ:
/// - **パストラバーサル防御**: `..`/絶対パス/ドライブ指定/バックスラッシュを含む参照は拒否する
///   （`include_dir::get_file` は仮想 FS で `..` を解決しないが、防御的に明示拒否も入れる＝多層防御）。
/// - 配信は `/assets/` 配下（[`PREVIEW_ASSETS`] の仮想ツリー）のみ。存在しなければ 404。
/// - `X-Content-Type-Options: nosniff` を付ける。アセット応答自体に CSP ヘッダは付けない
///   （ドキュメントのレスポンスヘッダ CSP が読み込み可否を統制する）。長期キャッシュ可。
fn asset_response(reference: &str) -> Response<Vec<u8>> {
    // パストラバーサル/絶対参照の明示拒否（仮想 FS の防御に加える多層防御）。
    if reference.is_empty()
        || reference.contains("..")
        || reference.starts_with('/')
        || reference.contains('\\')
        || reference.contains(':')
    {
        return error_response(StatusCode::FORBIDDEN, "不正なアセット参照");
    }

    // 仮想 FS から引く（区切りは `/` 固定。include_dir は相対パスで解決する）。
    let Some(file) = PREVIEW_ASSETS.get_file(reference) else {
        return error_response(StatusCode::NOT_FOUND, "アセットなし");
    };

    let content_type = guess_content_type(std::path::Path::new(reference));
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, content_type)
        .header("X-Content-Type-Options", "nosniff")
        // 同梱アセットはビルドで固定（再現性は vendor.lock）。長期キャッシュ可。
        .header(header::CACHE_CONTROL, "public, max-age=31536000, immutable")
        .body(file.contents().to_vec())
        .unwrap_or_else(|_| error_response(StatusCode::INTERNAL_SERVER_ERROR, "応答組立失敗"))
}

/// エラーレスポンス（本文を最小化し情報漏れを避ける）。
fn error_response(status: StatusCode, _detail: &str) -> Response<Vec<u8>> {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "text/plain; charset=utf-8")
        // CSP を最も厳格に（万一の本文も実行させない）。
        .header(header::CONTENT_SECURITY_POLICY, "default-src 'none'")
        .body(Vec::new())
        .unwrap_or_else(|_| Response::new(Vec::new()))
}

/// 系統B（HTML）の危険検知の純粋ロジック（要件6.3）。
///
/// `<script>` や Tailwind CDN・外部 http(s) リソース・`<meta refresh>` を検知する。これは
/// **防御層ではなく UX 補助**（実防御は ammonia + CSP・low 指摘 #6）。検知＝防御に昇格させない。
/// 1 回の lowercase で全判定を済ませる（performance）。prepare_preview から呼び二重走査を避ける。
fn scan_hazards(content: &str) -> HtmlHazards {
    let lower = content.to_ascii_lowercase();
    HtmlHazards {
        has_script: lower.contains("<script"),
        has_external_ref: lower.contains("http://") || lower.contains("https://"),
        has_meta_refresh: lower.contains("http-equiv") && lower.contains("refresh"),
    }
}

/// HTML プレビューの危険検知結果（要件6.3・通知バー文言の根拠）。
#[derive(Debug, Clone, Default, Serialize)]
pub struct HtmlHazards {
    /// `<script>` を含む（JS は CSP/ammonia で無効化済みだが「表示が崩れる」通知を出す）。
    pub has_script: bool,
    /// 外部 http(s) リソース参照を含む（既定で遮断・オプトイン許可導線）。
    pub has_external_ref: bool,
    /// `<meta http-equiv="refresh">` を含む（ammonia で除去済みだが通知する）。
    pub has_meta_refresh: bool,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn html_危険検知_script_と外部参照と_meta_refresh() {
        let h = scan_hazards(
            r#"<script src="https://cdn.tailwindcss.com"></script><meta http-equiv="refresh" content="0">"#,
        );
        assert!(h.has_script);
        assert!(h.has_external_ref);
        assert!(h.has_meta_refresh);
    }

    #[test]
    fn html_危険検知_安全な文書はすべて_false() {
        let h = scan_hazards("<div style=\"color:red\">こんにちは</div>");
        assert!(!h.has_script);
        assert!(!h.has_external_ref);
        assert!(!h.has_meta_refresh);
    }

    #[test]
    fn prepare_preview_の_hazards_は系統bでのみ検知し系統aは全false() {
        // IPC 二重転送回避（performance 指摘）の回帰防止: hazards は prepare_preview の戻りに同梱され、
        // 系統B でのみ検知・系統A（Markdown）は常に全 false であることを観測する。
        let dangerous = "<script>x()</script><meta http-equiv=\"refresh\" content=\"0\">";
        let b = scan_hazards(dangerous);
        assert!(b.has_script && b.has_meta_refresh);
        // 系統A は対象外で Default（全 false）を入れる方針。
        let a = HtmlHazards::default();
        assert!(!a.has_script && !a.has_external_ref && !a.has_meta_refresh);
    }

    #[test]
    fn prepare_html_preview_は_script_を除去する() {
        // pika-core の系統B サニタイズが配線で正しく呼ばれることを確認（多層の結線確認）。
        let resp = prepare_html_preview(
            "<script>evil()</script><p>ok</p>",
            &ExternalResourceAllow::blocked(),
        );
        assert!(!resp.body.contains("<script"));
        assert!(resp.csp.contains("script-src 'none'"));
    }

    /// PNG ヘッダ（IHDR の width/height）を組み立てる（配線テスト用）。
    fn png_header(w: u32, h: u32) -> Vec<u8> {
        let mut b = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
        b.extend_from_slice(&[0, 0, 0, 13]);
        b.extend_from_slice(b"IHDR");
        b.extend_from_slice(&w.to_be_bytes());
        b.extend_from_slice(&h.to_be_bytes());
        b.extend_from_slice(&[8, 2, 0, 0, 0]);
        b
    }

    #[test]
    fn guess_content_type_は_js_css_woff2_を正しく判定する() {
        // Stage ②: 同梱ベンダー JS の Content-Type（js => text/javascript）欠落の回帰防止。
        assert_eq!(
            guess_content_type(std::path::Path::new("highlight.min.js")),
            "text/javascript; charset=utf-8"
        );
        assert_eq!(
            guess_content_type(std::path::Path::new("katex.min.css")),
            "text/css; charset=utf-8"
        );
        assert_eq!(
            guess_content_type(std::path::Path::new("fonts/KaTeX_Main-Regular.woff2")),
            "font/woff2"
        );
    }

    #[test]
    fn asset_response_は同梱アセットを配信し_未知は404() {
        // 埋め込みツリーから実在アセットを引けること（mermaid/katex/highlight/フォント）。
        for (name, ct) in [
            ("mermaid.min.js", "text/javascript; charset=utf-8"),
            ("katex.min.js", "text/javascript; charset=utf-8"),
            ("highlight.min.js", "text/javascript; charset=utf-8"),
            ("katex-auto-render.min.js", "text/javascript; charset=utf-8"),
            ("katex.min.css", "text/css; charset=utf-8"),
            ("hljs-github-dark.min.css", "text/css; charset=utf-8"),
            ("fonts/KaTeX_Main-Regular.woff2", "font/woff2"),
        ] {
            let resp = asset_response(name);
            assert_eq!(resp.status(), StatusCode::OK, "{name} が配信されない");
            assert!(!resp.body().is_empty(), "{name} の本体が空");
            assert_eq!(
                resp.headers().get(header::CONTENT_TYPE).unwrap(),
                ct,
                "{name} の Content-Type が違う"
            );
            // nosniff を必ず付ける。
            assert_eq!(
                resp.headers().get("X-Content-Type-Options").unwrap(),
                "nosniff",
                "{name} に nosniff が無い"
            );
            // アセット応答自体に CSP を付けない（ドキュメントの CSP が統制する）。
            assert!(
                resp.headers()
                    .get(header::CONTENT_SECURITY_POLICY)
                    .is_none(),
                "{name} にアセット CSP を付けた"
            );
        }
        // 未知のアセットは 404。
        assert_eq!(
            asset_response("does-not-exist.js").status(),
            StatusCode::NOT_FOUND
        );
    }

    #[test]
    fn asset_response_はパストラバーサルを拒否する() {
        // `..`/絶対/ドライブ/バックスラッシュ参照は 403（仮想 FS の防御に加える明示拒否）。
        for bad in [
            "../secret.txt",
            "../../Cargo.toml",
            "fonts/../../etc/passwd",
            "/etc/passwd",
            "C:\\windows\\system32",
            "fonts\\..\\katex.min.js",
            "",
        ] {
            assert_eq!(
                asset_response(bad).status(),
                StatusCode::FORBIDDEN,
                "危険なアセット参照 `{bad}` が拒否されなかった"
            );
        }
    }

    #[test]
    fn ローカル配信の暴走ガードが結線されている() {
        // high 指摘の回帰防止: 配信前ガード（guard_local_resource）が pika-core を呼び、
        // 巨大画像/SVG をブロック・通常サイズを許可することを配線レベルで観測する。
        let big = png_header(10_000, 7_000); // 7000万px > 6000万px
        assert!(
            matches!(
                guard_local_resource("image/png", &big),
                GuardDecision::Block(_)
            ),
            "巨大画像が配信ガードを通過した"
        );
        let ok = png_header(800, 600);
        assert!(
            guard_local_resource("image/png", &ok).is_allowed(),
            "通常画像がブロックされた"
        );

        let big_svg = {
            let mut s = String::from("<svg>");
            for _ in 0..60_000 {
                s.push_str("<rect/>");
            }
            s.push_str("</svg>");
            s.into_bytes()
        };
        assert!(
            matches!(
                guard_local_resource("image/svg+xml", &big_svg),
                GuardDecision::Block(_)
            ),
            "巨大 SVG が配信ガードを通過した"
        );

        // 画像/SVG 以外（CSS）は対象外で常に許可（巨大でもデコード爆発しない）。
        assert!(guard_local_resource("text/css; charset=utf-8", &vec![0u8; 1024]).is_allowed());
    }
}
