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

use pika_core::render::{
    confine_under, join_under, prepare_html_preview, prepare_markdown_preview, resolve_local_ref,
    ExternalResourceAllow, LocalRefDecision, PreviewFlavor, PreviewResponse,
};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;
use tauri::http::{header, Response, StatusCode};
use tauri::{State, UriSchemeContext};

/// プレビュー custom protocol のスキーム名。
pub const PREVIEW_SCHEME: &str = "pika-preview";

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
    let response = match mode {
        PreviewMode::Markdown => prepare_markdown_preview(&content, &allow),
        PreviewMode::Html => prepare_html_preview(&content, &allow),
    };

    // ローカル相対参照の基準ディレクトリ（canonicalize して prefix 検証の基準にする＝design doc 6章）。
    let base_dir = PathBuf::from(&path)
        .parent()
        .and_then(|p| std::fs::canonicalize(p).ok());

    let mut inner = preview.inner.lock().map_err(|_| "preview ロック失敗")?;
    inner.generation += 1;
    let generation = inner.generation;
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
    })
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
    use tauri::Manager;
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
fn document_response(doc: &PreparedDoc) -> Response<Vec<u8>> {
    // CSP は pika-core が組んだ値（既定外部遮断・nonce・オプトイン緩和は img/font のみ）。
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "text/html; charset=utf-8")
        .header(header::CONTENT_SECURITY_POLICY, &doc.response.csp)
        // クリックジャッキング/MIME スニッフィング/参照漏れの追加防御。
        .header("X-Content-Type-Options", "nosniff")
        .header(header::REFERRER_POLICY, "no-referrer")
        .body(doc.response.body.clone().into_bytes())
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
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, content_type)
        .header("X-Content-Type-Options", "nosniff")
        .body(bytes)
        .unwrap_or_else(|_| error_response(StatusCode::INTERNAL_SERVER_ERROR, "応答組立失敗"))
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
        "woff" => "font/woff",
        "woff2" => "font/woff2",
        "ttf" => "font/ttf",
        _ => "application/octet-stream",
    }
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

/// 系統B（HTML・JS無効）プレビューの「JS/外部参照検知」を frontend に伝える（要件6.3・通知バー導線）。
///
/// `<script>` や Tailwind CDN・外部 http(s) リソースを検知したら通知バーで「既定のブラウザで開く」へ
/// 誘導する（要件6.3）。検知ロジックは軽量で純粋なため command 内で素朴に走査する。
#[tauri::command]
pub fn scan_html_hazards(content: String) -> HtmlHazards {
    let lower = content.to_ascii_lowercase();
    HtmlHazards {
        has_script: lower.contains("<script"),
        has_external_ref: lower.contains("http://") || lower.contains("https://"),
        has_meta_refresh: lower.contains("http-equiv") && lower.contains("refresh"),
    }
}

/// HTML プレビューの危険検知結果（要件6.3・通知バー文言の根拠）。
#[derive(Debug, Clone, Serialize)]
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
        let h = scan_html_hazards(
            r#"<script src="https://cdn.tailwindcss.com"></script><meta http-equiv="refresh" content="0">"#
                .to_string(),
        );
        assert!(h.has_script);
        assert!(h.has_external_ref);
        assert!(h.has_meta_refresh);
    }

    #[test]
    fn html_危険検知_安全な文書はすべて_false() {
        let h = scan_html_hazards("<div style=\"color:red\">こんにちは</div>".to_string());
        assert!(!h.has_script);
        assert!(!h.has_external_ref);
        assert!(!h.has_meta_refresh);
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
}
