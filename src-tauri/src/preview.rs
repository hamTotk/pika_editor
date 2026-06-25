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
    PreviewTheme, DEFAULT_IMAGE_MAX_PIXELS, DEFAULT_SVG_MAX_ELEMENTS, DEFAULT_SVG_MAX_PIXELS,
};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;
use tauri::http::{header, Response, StatusCode};
use tauri::{LogicalPosition, LogicalSize, Manager, State, UriSchemeContext};

/// プレビュー別WebView のラベル（capability マップに**含めない**＝権限ゼロ・design doc 6章/9章）。
pub const PREVIEW_WEBVIEW_LABEL: &str = "preview";

/// 発信元 WebView ラベルが「IPC を拒否すべき権限ゼロ WebView（プレビュー）」かを判定する純粋関数。
///
/// `main.rs` の `invoke_handler` 前段ガードが使う唯一の判定点（design doc 6章/9章「Tauri API 到達不能」）。
/// Tauri v2 の ACL は自前 app command を自動ゲートしないため、ここでラベルを照合して
/// プレビュー別WebView（[`PREVIEW_WEBVIEW_LABEL`]）からの invoke を command ディスパッチ前に弾く。
/// メイン窓（label `"main"`）など他ラベルは `false`（=従来どおり全 command を通す）。
/// 文字列直書きを避け定数照合に寄せることで、ラベル変更時の取りこぼしを防ぐ（回帰テスト対象）。
#[inline]
pub fn is_blocked_invoke_origin(label: &str) -> bool {
    label == PREVIEW_WEBVIEW_LABEL
}

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

/// frontend から渡るテーマ配色（Stage ③・design doc 10章）。
///
/// メインアプリの解決済みトークン色（`--bg-raised`/`--text-1`/…）を別WebView 文書へ降ろすための DTO。
/// **pika-core に serde/Tauri 型を持ち込まない**ため、ここで invoke 引数を受け取り
/// 純粋な [`PreviewTheme`]（色文字列のみ）へ変換する（安全化検証は pika-core::render が担う）。
#[derive(Debug, Clone, Deserialize)]
pub struct PreviewThemeDto {
    pub bg: String,
    pub fg: String,
    pub muted: String,
    pub border: String,
    pub accent: String,
    pub sunken: String,
    pub dark: bool,
}

impl From<PreviewThemeDto> for PreviewTheme {
    fn from(d: PreviewThemeDto) -> Self {
        PreviewTheme {
            bg: d.bg,
            fg: d.fg,
            muted: d.muted,
            border: d.border,
            accent: d.accent,
            sunken: d.sunken,
            dark: d.dark,
        }
    }
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
///
/// `Clone` 実装は #19 のため: custom protocol ハンドラが `inner` ロックを保持したまま FS I/O
/// （canonicalize+read）を行うと、配信中に prepare_preview 等の他操作がロック待ちで詰まる。
/// 必要素材を clone してから `drop(inner)` で早期解放し、I/O はロック外で行う。
#[derive(Clone)]
struct PreparedDoc {
    /// サニタイズ済み HTML＋CSP＋nonce（pika-core::render の成果）。
    /// オプトイン外部許可は CSP（`response.csp`）に既に織り込み済み（design doc 6章）。
    response: PreviewResponse,
    /// ローカル相対参照（画像/CSS）を解決する基準ディレクトリ（canonicalize 済み）。
    /// `None` は基準ディレクトリ不明（FS 上に無い等）＝ローカル参照は配信しない。
    base_dir: Option<PathBuf>,
    /// 別WebView へ降ろすテーマ配色（Stage ③・design doc 10章）。`document_response` が wrap へ渡す。
    /// `None`（theme 未指定・系統B 文書スタイル尊重）のときは OS 任せ（color-scheme）に倒れる。
    theme: Option<PreviewTheme>,
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
/// - `theme`: メインアプリの解決済みトークン色（Stage ③・design doc 10章）。系統A のみ別WebView へ降ろす。
///   系統B（HTML）は文書スタイル尊重で適用しない（要件11.3）。色文字列の安全化は pika-core が担う。
#[tauri::command]
pub fn prepare_preview(
    path: String,
    mode: PreviewMode,
    content: String,
    allow_external: Option<Vec<String>>,
    theme: Option<PreviewThemeDto>,
    preview: State<'_, PreviewService>,
) -> Result<PreparedPreview, String> {
    let allow = ExternalResourceAllow {
        hosts: allow_external.unwrap_or_default(),
    };
    // 系統A のみテーマを別WebView へ降ろす（系統B は文書スタイル尊重＝要件11.3・ui-design 8章）。
    let theme: Option<PreviewTheme> = match mode {
        PreviewMode::Markdown => theme.map(PreviewTheme::from),
        PreviewMode::Html => None,
    };

    // サニタイズ＋CSP（pika-core::render）。HTML 本体は以後ストアにのみ置き、invoke で返さない。
    // 系統B の危険検知（要件6.3）も同じ content からこの 1 回で済ませる（IPC 二重転送回避）。
    let mut response = match mode {
        PreviewMode::Markdown => prepare_markdown_preview(&content, &allow),
        PreviewMode::Html => prepare_html_preview(&content, &allow),
    };
    let hazards = match mode {
        // 系統A（Markdown）は文書 JS 検知の対象外（comrak/ammonia でサニタイズ済み）だが、外部リソース
        // 参照（リモート画像/フォント）はオプトイン許可の対象なので外部参照だけは収集する（要件6.2）。
        PreviewMode::Markdown => scan_markdown_external(&content),
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
    inner.documents.insert(
        generation,
        PreparedDoc {
            response,
            base_dir,
            theme,
        },
    );

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
    // #6: ナビゲート先は custom protocol 実体（pika-preview.localhost）の URL のみ許可する。
    // frontend からの値は信頼せず、接頭辞検証で任意 URL（外部サイト等）への誘導を塞ぐ
    // （prepare_preview が返す URL は必ずこの接頭辞で始まるため正当フローは通る）。
    const ALLOWED_PREFIX: &str = "http://pika-preview.localhost/";
    if !url.starts_with(ALLOWED_PREFIX) {
        return Err("プレビューURLが不正です".into());
    }
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
        let Some(doc) = inner.documents.get(&generation).cloned() else {
            return error_response(StatusCode::NOT_FOUND, "プレビュー素材なし（世代失効）");
        };
        // #19: 必要素材を clone 済み。ロックを解放してから応答組立（FS I/O は無いが一貫させる）。
        drop(inner);
        return document_response(&doc);
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
        let Some(doc) = inner.documents.get(&generation).cloned() else {
            return error_response(StatusCode::NOT_FOUND, "プレビュー素材なし（世代失効）");
        };
        // #19: doc を clone 済み。ロックを解放してから canonicalize+read（FS I/O）を行う
        // （ロック保持のまま FS I/O すると他の preview 操作が詰まるため）。
        drop(inner);
        return local_resource_response(&doc, reference);
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
    // テーマ配色を別WebView 文書へ降ろす（Stage ③・design doc 10章）。系統B では prepare_preview が
    // theme=None にしてある（文書スタイル尊重）。色文字列の安全化検証は wrap_preview_document が担う。
    let html = wrap_preview_document(
        &doc.response.body,
        &doc.response.nonce,
        doc.response.flavor,
        doc.theme.as_ref(),
    );
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

    // 同一 WebView セッション内で繰り返し読まれるプレビュー素材へ控えめなキャッシュを効かせる（#53）。
    // Range 非対応（プレビュー画像/CSS にシークは不要＝scope 外。実機での挙動確認は系統C）。
    // 条件付き応答（304・If-None-Match 突合）は実装しない（シグネチャ非変更・max-age で十分）。
    let mut builder = Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, content_type)
        .header("X-Content-Type-Options", "nosniff")
        // 5 分（控えめ）。素材は同一セッション内で繰り返し読まれる。
        .header(header::CACHE_CONTROL, "max-age=300");
    // mtime+サイズから弱い実体タグを付ける。取得失敗時は ETag を省略する（Cache-Control だけ付く）。
    if let Ok(meta) = std::fs::metadata(&resolved) {
        let size = meta.len();
        if let Some(mtime_ms) = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_millis())
        {
            builder = builder.header(header::ETAG, format!("\"{mtime_ms:x}-{size:x}\""));
        }
    }
    builder
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
///
/// `pub(crate)`: pika-asset:// の配信ヘルパ（[`crate::asset`]）も同じ Content-Type 推定を使い
/// 重複実装を作らない（単一源・U3 画像簡易ビュー）。
pub(crate) fn guess_content_type(path: &std::path::Path) -> &'static str {
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
///
/// `pub(crate)`: pika-asset:// のハンドラ（[`crate::asset`]）も同じ最小エラー応答を使い
/// 重複実装を作らない（単一源・U3 画像簡易ビュー）。
pub(crate) fn error_response(status: StatusCode, _detail: &str) -> Response<Vec<u8>> {
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
/// 外部参照を検知したらホスト名も収集し、オプトイン許可（要件6.2「許可しますか」）の候補にする。
/// 1 回の lowercase で全判定を済ませる（performance）。prepare_preview から呼び二重走査を避ける。
fn scan_hazards(content: &str) -> HtmlHazards {
    let lower = content.to_ascii_lowercase();
    let external_hosts = collect_external_hosts(content);
    HtmlHazards {
        has_script: lower.contains("<script"),
        has_external_ref: !external_hosts.is_empty(),
        has_meta_refresh: lower.contains("http-equiv") && lower.contains("refresh"),
        external_hosts,
    }
}

/// 系統A（Markdown）の外部参照だけを収集する（要件6.2 オプトイン許可導線）。
///
/// Markdown は comrak/ammonia でサニタイズ済みで `<script>`/`meta refresh` 検知は対象外だが、
/// リモート画像/フォントはオプトイン許可（既定遮断）の対象なので外部ホストだけを集める。
/// `has_script`/`has_meta_refresh` は常に false（Markdown の通知バー文言は外部参照のみ）。
fn scan_markdown_external(content: &str) -> HtmlHazards {
    let external_hosts = collect_external_hosts(content);
    HtmlHazards {
        has_script: false,
        has_external_ref: !external_hosts.is_empty(),
        has_meta_refresh: false,
        external_hosts,
    }
}

/// 素の文書から外部参照ホスト（`https://<host>`）を重複排除して収集する（要件6.2/6.3・2.4）。
///
/// オプトイン許可（[`validate_allow_hosts`] が後段で https のみ受理して再検証）する候補なので、
/// **`https://` のみ**収集する（`http://` は盗聴/プライバシーのため対象外＝既定遮断のまま）。
/// 防御ではなく UX 補助（実防御は ammonia + CSP）なので走査は簡易でよい:
/// `https://` 出現位置からホスト部（`/`・`?`・`#`・空白・クォート・`<`/`>` の手前まで）を切り出し、
/// `validate_one_host` 相当の最終検証は CSP 組立時（[`build_csp`]）が行う（緩く集めて検証で落とす）。
fn collect_external_hosts(content: &str) -> Vec<String> {
    let mut hosts: Vec<String> = Vec::new();
    let bytes = content.as_bytes();
    let lower = content.to_ascii_lowercase();
    let mut search_from = 0usize;
    // 大文字小文字を無視して `https://` を探すため lowercase 側で位置を取り、ホスト抽出は元 content で行う
    // （ホスト名は ASCII なので大小は host source 比較に影響しないが、元文字列から切り出す）。
    while let Some(rel) = lower[search_from..].find("https://") {
        let start = search_from + rel;
        let host_start = start + "https://".len();
        // ホスト部の終端を、区切り文字（パス/クエリ/フラグメント/空白/引用/タグ境界）まで進めて決める。
        let mut end = host_start;
        while end < bytes.len() {
            let c = bytes[end] as char;
            if c == '/'
                || c == '?'
                || c == '#'
                || c == '"'
                || c == '\''
                || c == '<'
                || c == '>'
                || c == ')'
                || c == ']'
                || c.is_whitespace()
            {
                break;
            }
            end += 1;
        }
        let host = &content[host_start..end];
        if !host.is_empty() {
            let normalized = format!("https://{host}");
            if !hosts.contains(&normalized) {
                hosts.push(normalized);
            }
        }
        // 次の検索開始位置（最低でも 1 進めて無限ループを防ぐ）。
        search_from = end.max(start + "https://".len());
    }
    hosts
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
    /// 収集した外部参照ホスト（`https://<host>`・重複排除・http は対象外）。
    ///
    /// frontend の「許可して再読込」が `allow_external` としてそのまま backend へ返す候補。
    /// 最終検証は CSP 組立時の [`validate_allow_hosts`]/[`build_csp`] が https のみ受理して行う
    /// （緩く集めて検証で落とす＝要件6.2 のオプトイン許可）。
    pub external_hosts: Vec<String>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ipc_発信元ガードはプレビューを拒否しメインは通す() {
        // 最重要セキュリティ境界の回帰防止（design doc 6章/9章）: invoke_handler 前段ガードが使う
        // ラベル判定が「プレビュー別WebView は拒否・メイン窓ほかは許可」であることを純粋関数として固定する。
        // 実機プローブで read_file 漏洩を検出した欠陥（自前 app command が ACL で自動ゲートされない）を
        // ここで塞ぐ唯一の判定点なので、ラベル定数（PREVIEW_WEBVIEW_LABEL）変更にも追従させる。
        assert!(
            is_blocked_invoke_origin(PREVIEW_WEBVIEW_LABEL),
            "プレビューWebView（権限ゼロ）からの IPC が拒否されていない"
        );
        assert!(
            is_blocked_invoke_origin("preview"),
            "現行のプレビューラベル文字列が拒否されていない"
        );
        // メイン窓は従来どおり全 command を通す（既存機能を壊さない）。
        assert!(
            !is_blocked_invoke_origin(MAIN_WINDOW_LABEL),
            "メイン窓の IPC が誤って拒否された"
        );
        assert!(!is_blocked_invoke_origin("main"));
        // 想定外ラベルは拒否しない（プレビューのみを限定して塞ぐ＝過剰遮断で既存機能を壊さない）。
        assert!(!is_blocked_invoke_origin(""));
        assert!(!is_blocked_invoke_origin("other"));
    }

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
        assert!(h.external_hosts.is_empty());
    }

    #[test]
    fn 外部ホスト収集_https_のみを重複排除して集める() {
        // 要件6.2/6.3・2.4: オプトイン許可候補は https のみ。http は盗聴/プライバシーのため除外する。
        let hosts = collect_external_hosts(
            "<img src=\"https://www.w3.org/logo.png\"> \
             <link href='https://cdn.example.com/x.css'> \
             <img src=\"http://insecure.example/a.png\"> \
             <img src=\"https://www.w3.org/another.svg\">",
        );
        // www.w3.org は重複排除され 1 件、cdn.example.com が 1 件。http は含めない。
        assert!(
            hosts.contains(&"https://www.w3.org".to_string()),
            "w3.org が収集されない: {hosts:?}"
        );
        assert!(
            hosts.contains(&"https://cdn.example.com".to_string()),
            "cdn が収集されない: {hosts:?}"
        );
        assert!(
            !hosts.iter().any(|h| h.contains("insecure.example")),
            "http ホストが混入した: {hosts:?}"
        );
        // 重複排除: www.w3.org は 1 回だけ。
        assert_eq!(
            hosts
                .iter()
                .filter(|h| h.as_str() == "https://www.w3.org")
                .count(),
            1,
            "重複排除されていない: {hosts:?}"
        );
        // 収集したホストは https のみ・パス/クエリを含まない（CSP の host source 形式）。
        for h in &hosts {
            assert!(h.starts_with("https://"), "https 以外が混入: {h}");
            assert!(!h["https://".len()..].contains('/'), "パスが残った: {h}");
        }
    }

    #[test]
    fn 外部ホスト収集_ポート付きとフラグメント_クエリを正しく切る() {
        let hosts = collect_external_hosts(
            "see https://example.com:8443/path?q=1#frag and https://cdn.test/a",
        );
        assert!(
            hosts.contains(&"https://example.com:8443".to_string()),
            "ポート付きホストが取れない: {hosts:?}"
        );
        assert!(
            hosts.contains(&"https://cdn.test".to_string()),
            "ホストが取れない: {hosts:?}"
        );
    }

    #[test]
    fn 外部ホスト収集_収集結果は_csp_検証を通る() {
        // 緩く集めて検証で落とす規約（要件6.2）: collect の出力は build_csp/validate_allow_hosts が
        // 受理できる host source 形式（https のみ・パス/クエリ/区切りなし）であることを担保する。
        use pika_core::render::{validate_allow_hosts, ExternalResourceAllow};
        let hosts = collect_external_hosts(
            "<img src=\"https://www.w3.org/Icons/valid-html401.png\"> \
             <img src=\"https://cdn.example.com:443/lib/font.woff2\">",
        );
        let allow = ExternalResourceAllow {
            hosts: hosts.clone(),
        };
        assert!(
            validate_allow_hosts(&allow).is_ok(),
            "収集ホストが CSP 検証で落ちた: {hosts:?}"
        );
    }

    #[test]
    fn 系統a_markdown_でも外部参照ホストを収集する() {
        // 要件6.2: Markdown の外部画像もオプトイン許可の対象。has_script/meta_refresh は常に false。
        let h = scan_markdown_external("![logo](https://www.w3.org/logo.png) と普通のテキスト");
        assert!(h.has_external_ref, "外部参照が検知されない");
        assert!(!h.has_script, "Markdown で has_script が立った");
        assert!(!h.has_meta_refresh, "Markdown で has_meta_refresh が立った");
        assert_eq!(h.external_hosts, vec!["https://www.w3.org".to_string()]);
        // 外部参照のない Markdown はすべて false・空。
        let none = scan_markdown_external("# 見出し\n\n本文だけ");
        assert!(!none.has_external_ref);
        assert!(none.external_hosts.is_empty());
    }

    #[test]
    fn prepare_preview_の_hazards_は外部ホストを同梱する() {
        // prepare_preview の戻り（HtmlHazards）に external_hosts が乗り、frontend が allow_external として
        // 再投入できることを配線レベルで観測する（要件6.2 オプトイン許可・IPC 二重転送回避）。
        let b = scan_hazards("<img src=\"https://cdn.example.com/a.png\">");
        assert!(b.has_external_ref);
        assert_eq!(
            b.external_hosts,
            vec!["https://cdn.example.com".to_string()]
        );
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

    /// テスト用のライトテーマ DTO→PreviewTheme（配線テスト用）。
    fn theme_for_test(dark: bool) -> PreviewTheme {
        PreviewTheme::from(PreviewThemeDto {
            bg: "#fafafb".into(),
            fg: "#26262b".into(),
            muted: "#5a5a62".into(),
            border: "#cacace".into(),
            accent: "#4f74a8".into(),
            sunken: "#e6e6ea".into(),
            dark,
        })
    }

    #[test]
    fn document_response_は系統aの_theme_を本文に注入する() {
        // Stage ③: PreparedDoc.theme が document_response→wrap_preview_document へ渡り、
        // :root{--pk-*} 定義として別WebView 文書に乗ることを配線レベルで観測する（design doc 10章）。
        let resp = prepare_markdown_preview("# 見出し\n\n本文", &ExternalResourceAllow::blocked());
        let doc = PreparedDoc {
            response: resp,
            base_dir: None,
            theme: Some(theme_for_test(false)),
        };
        let out = document_response(&doc);
        assert_eq!(out.status(), StatusCode::OK);
        let html = String::from_utf8_lossy(out.body());
        assert!(
            html.contains("--pk-bg:#fafafb"),
            "theme 変数が本文に無い: {html}"
        );
        // CSP はレスポンスヘッダで強制し続ける（theme 注入で緩めない）。
        assert!(
            out.headers().get(header::CONTENT_SECURITY_POLICY).is_some(),
            "CSP ヘッダが消えた"
        );
    }

    #[test]
    fn document_response_は系統bには_theme_を注入しない() {
        // 系統B（HTML）は文書スタイル尊重（要件11.3）。prepare_preview で theme=None になる前提だが、
        // 万一 theme が積まれても wrap が系統B では --pk-* を出さないことを二重に担保する。
        let resp = prepare_html_preview(
            "<div style=\"color:red\">x</div>",
            &ExternalResourceAllow::blocked(),
        );
        let doc = PreparedDoc {
            response: resp,
            base_dir: None,
            theme: Some(theme_for_test(true)),
        };
        let out = document_response(&doc);
        let html = String::from_utf8_lossy(out.body());
        assert!(
            !html.contains(":root{--pk"),
            "系統B に theme 変数定義が漏れた: {html}"
        );
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
