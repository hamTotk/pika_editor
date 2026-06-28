//! 画像簡易ビューの backend（判定 command＋専用 custom protocol・要件12.2・U3）。
//!
//! 役割（design doc 3章「薄い境界」）:
//! - 画像かどうか・寸法上限/ファイル上限の判定（[`image_info`]）と、画像バイトの別配信
//!   （`pika-asset://` custom protocol＝[`handle_asset_request`]）をメインWebView へ提供する。
//! - 寸法読取り（ヘッダプリチェック）・暴走ガード・寸法上限判定は **すべて pika-core::render / nontext**
//!   （cargo test 済み）に委ね、ここは FS 読取と DTO 化・封じ込め検証の結線に徹する。
//! - 画像 HTML（`<img src>`）は invoke で返さない。frontend は `pika-asset://` の URL を `<img>` に張るだけ
//!   （IPC 予算＋大バイト転送回避＝design doc 1章「軽い/固まらない」）。
//!
//! **セキュリティ最重要（不変条件・隔離の関門）**:
//! この protocol は `main.rs` で**アプリ全体に登録**するため、原理上は権限ゼロのプレビュー別WebView からも
//! 到達しうる（発信元 origin に依存しない）。漏洩を塞ぐ関門は三重で、いずれも origin に依らない:
//!   (a) [`AccessControl::verify_read`] の path ゲート（絶対パス検証＋canonicalize＋root/allowed 封じ込め）。
//!       自前のパス正規化/許可判定はここでは一切書かず、唯一の関門を AccessControl に委ねる。
//!   (b) 解決後の実体名で `is_sensitive` を再判定（機密ファイルはシンボリックリンク偽装も含め配信拒否）。
//!   (c) プレビュー別WebView の CSP（`preview::build_csp` 系統）に `pika-asset` を**入れない**
//!       （未信頼文書から画像 protocol を引く面を CSP で塞ぐ＝`tauri.conf.json` でメインWebView のみ許可）。
//!
//! 設計原則「固まらない」: 寸法が小さくてもファイル実体が巨大な細工画像で転送/デコードが爆発しないよう、
//! 判定（[`image_info`]）と配信（[`serve_verified_image`]）で**同じファイル上限**（[`MAX_IMAGE_FILE_BYTES`]）を
//! 使い、上限超は中身を読まずに弾く（判定と配信の挙動を一致させる）。

use percent_encoding::percent_decode_str;
use pika_core::nontext::{decide_image_open, ImageOpenDecision};
use pika_core::render::{
    check_image_bytes, image_dimensions, GuardDecision, DEFAULT_IMAGE_MAX_PIXELS,
};
use std::path::Path;
use tauri::http::{header, Request, Response, StatusCode};
use tauri::{Manager, State, UriSchemeContext, Wry};

use crate::access::AccessControl;
use crate::preview::{
    deny_if_sensitive, error_response, finish_response, guess_content_type, ok_response_builder,
};

/// 画像配信 custom protocol のスキーム名（実体 `http://pika-asset.localhost/`）。
pub const ASSET_SCHEME: &str = "pika-asset";

/// 画像ファイルの実体バイト上限（要件12.2・「固まらない」ガード）。
///
/// 寸法ヘッダが小さくてもファイル実体が巨大な細工画像があり得るため、寸法プリチェックとは別に
/// **ファイルサイズ自体**でも弾く（転送/デコード前に metadata で判断し中身を読まない）。
/// [`image_info`] と [`serve_verified_image`] で同じ上限を使い、判定と配信の挙動を一致させる。
pub const MAX_IMAGE_FILE_BYTES: u64 = 64 * 1024 * 1024;

/// [`image_info`] の判定用に読むヘッダプレフィックス長（64 KiB）。
///
/// 寸法は [`image_dimensions`] がヘッダ先頭から読むため、PNG/GIF/BMP/ICO/WEBP/一般的な JPEG は
/// 64 KiB のプレフィックス（先頭の寸法マーカーまで）で十分に寸法が得られる。判定（[`image_info`]）と
/// 配信（[`serve_verified_image`]）が**それぞれ最大 64MiB をフルリード**して画像1枚で二重に全読みして
/// いたのを、判定側は先頭プレフィックスのみに縮小する（指摘2・設計原則「軽い／固まらない」）。
/// 極端に寸法マーカーが深い JPEG では寸法不明→[`ImageInfoDto::TooLarge`]/[`ImageInfoDto::Unsupported`]
/// へ倒れるだけで安全側（誤って巨大画像を描画しない＝fail-closed）。
const IMAGE_HEADER_PREFIX_BYTES: u64 = 64 * 1024;

/// ファイル先頭の最大 `limit` バイトを読む（ヘッダ判定用・全読みを避ける＝固まらない）。
///
/// `File::open` + `Read::take(limit)` で上限付きに読み、巨大ファイルでも判定のために全バイトを
/// メモリへ載せない。寸法読取りに足りるプレフィックスだけを返す（[`IMAGE_HEADER_PREFIX_BYTES`]）。
fn read_prefix(path: &Path, limit: u64) -> std::io::Result<Vec<u8>> {
    use std::io::Read;
    let file = std::fs::File::open(path)?;
    let mut buf = Vec::new();
    file.take(limit).read_to_end(&mut buf)?;
    Ok(buf)
}

/// `image_info` command の戻り DTO（frontend と round-trip・serde）。
///
/// `kind` タグで分岐する（serde の internally tagged enum・kebab-case）:
/// - `"image"`: 画像として描ける（寸法上限内・ファイル上限内・既知画像マジック）。
/// - `"too-large"`: 寸法上限超 or 寸法不明 or ファイル上限超（外部誘導へ倒す）。
/// - `"unsupported"`: 既知画像として解釈できない（画像として描けない＝外部誘導/非対応表示）。
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum ImageInfoDto {
    /// 画像として簡易ビューで描いてよい（寸法/ファイル上限内）。
    Image {
        /// ヘッダから読んだ幅（px）。
        width: u64,
        /// ヘッダから読んだ高さ（px）。
        height: u64,
        /// 配信時に付く Content-Type（拡張子由来・`guess_content_type` と一致）。
        mime: String,
    },
    /// 寸法上限超 or 寸法不明 or ファイル上限超（外部誘導へ倒す＝固まらない）。
    TooLarge {
        /// 通知文言用の総ピクセル数（寸法不明/ファイル上限超では 0）。
        pixels: u64,
    },
    /// 既知画像として解釈できない（画像として描けない＝非対応表示/外部誘導）。
    Unsupported,
}

/// 解決済みパスと画像バイトから [`ImageInfoDto`] を決める純粋ヘルパ（cargo test 対象）。
///
/// 寸法読取り（[`image_dimensions`]）と寸法上限判定（[`decide_image_open`]）は pika-core（cargo test 済み）。
/// ここは「読めた寸法をどの DTO に畳むか」の結線のみで FS には触れない（呼び出し側が読んだバイトを渡す）。
///
/// - 寸法 `None`（既知画像マジックに当たらない）→ [`ImageInfoDto::Unsupported`]（画像として描けない）。
/// - 寸法 `Some((w, h))`:
///   - **u64→u32 変換**: `decide_image_open` は u32 を取る。w/h が u32 上限を超える異常寸法は
///     `try_into` 失敗とみなし [`ImageInfoDto::TooLarge`]`{ pixels: 0 }`（寸法異常として外部誘導）。
///   - [`ImageOpenDecision::Decode`] → [`ImageInfoDto::Image`]（mime は拡張子由来）。
///   - [`ImageOpenDecision::OpenExternally`] → [`ImageInfoDto::TooLarge`]（寸法上限超）。
///   - [`ImageOpenDecision::DimensionsUnknown`] → [`ImageInfoDto::TooLarge`]`{ pixels: 0 }`。
fn image_info_for(resolved: &Path, bytes: &[u8]) -> ImageInfoDto {
    let Some((w, h)) = image_dimensions(bytes) else {
        // 既知画像マジックに当たらない＝画像として描けない（非対応バイナリ扱い・外部誘導）。
        return ImageInfoDto::Unsupported;
    };

    // u64→u32 変換に注意（要件・タスク指定）: u32 上限超の異常寸法は寸法異常として TooLarge に倒す。
    let (Ok(w32), Ok(h32)) = (u32::try_from(w), u32::try_from(h)) else {
        return ImageInfoDto::TooLarge { pixels: 0 };
    };

    match decide_image_open(Some((w32, h32))) {
        ImageOpenDecision::Decode { .. } => ImageInfoDto::Image {
            width: w,
            height: h,
            mime: guess_content_type(resolved).to_string(),
        },
        ImageOpenDecision::OpenExternally { pixels } => ImageInfoDto::TooLarge { pixels },
        ImageOpenDecision::DimensionsUnknown => ImageInfoDto::TooLarge { pixels: 0 },
    }
}

/// 画像かどうか・寸法/ファイル上限を判定する command（要件12.2・U3）。
///
/// frontend が画像/非対応バイナリ/巨大画像を分岐するための判定だけを返す（画像バイトは別 protocol で配信）。
///
/// 多層防御（順序固定・最重要不変条件）:
/// 1. [`AccessControl::verify_read`] で封じ込め（任意パスを判定対象にしない）。
/// 2. metadata のサイズが [`MAX_IMAGE_FILE_BYTES`] 超 → 中身を読まず [`ImageInfoDto::TooLarge`]（外部誘導）。
/// 3. 機密再判定（`is_sensitive_with`＝設定パターン和集合・既定は外せない）に当たれば**エラー**で拒否する
///    （機密ファイルは表示自体させない）。state 未登録時は空 patterns＝既定のみで安全側。
/// 4. 読んだバイトを [`image_info_for`] へ畳んで返す。
#[tauri::command]
pub fn image_info(
    path: String,
    access: State<'_, AccessControl>,
    settings: State<'_, std::sync::Arc<crate::settings_service::SettingsService>>,
) -> Result<ImageInfoDto, String> {
    // (1) 封じ込め（path ゲート＝唯一の関門に委ねる）。
    let resolved = access.verify_read(&path)?;

    // (2) ファイル上限を metadata で先に弾く（巨大ファイルは中身を読まない＝固まらない）。
    let size = std::fs::metadata(&resolved)
        .map_err(|e| format!("メタデータ取得に失敗: {e}"))?
        .len();
    if size > MAX_IMAGE_FILE_BYTES {
        return Ok(ImageInfoDto::TooLarge { pixels: 0 });
    }

    // (3) 機密再判定（解決後の実体名で・シンボリックリンク偽装も塞ぐ）。機密は表示自体させない。
    // 機密判定は既定 ∪ 設定 sensitive_patterns（既定は外せない＝U2b-2）。
    if pika_core::snapshot::policy::is_sensitive_with(
        &resolved.to_string_lossy(),
        &settings.snapshot().sensitive_patterns,
    ) {
        return Err("機密ファイルは表示できません".into());
    }

    // (4) ヘッダプレフィックスのみ読んで判定（寸法はヘッダ先頭から・フルデコードも全読みもしない＝指摘2）。
    // 配信（serve_verified_image）は全バイトが要るので別途読むが、判定側はここで全読みしない。
    let bytes = read_prefix(&resolved, IMAGE_HEADER_PREFIX_BYTES)
        .map_err(|e| format!("読み込みに失敗: {e}"))?;
    Ok(image_info_for(&resolved, &bytes))
}

/// 封じ込め検証済みの実体パスから画像バイトを配信する（[`local_resource_response`] と同順の防御）。
///
/// `verify_read` は**呼び出し側（[`handle_asset_request`]）が済ませた前提**で resolved を受ける
/// （封じ込めは protocol ハンドラの唯一の関門で一度だけ通す）。配信前の各段は preview.rs の
/// `local_resource_response` と同じ順序・同じ責務に揃える（単一系統の防御順を崩さない）:
/// 1. 機密再判定（`is_sensitive_with`＝設定パターン和集合・既定は外せない）→ 403。
/// 2. ファイル上限超（[`MAX_IMAGE_FILE_BYTES`]）→ 413（中身を読まず metadata で弾く）。
/// 3. 読取り失敗 → 404。
/// 4. 暴走ガード（[`check_image_bytes`]・寸法不明/寸法上限超）→ 413（デコード前に弾く＝固まらない）。
/// 5. OK → Content-Type＋nosniff＋控えめキャッシュ（5分）で 200 配信。
///
/// `sensitive_patterns` は設定 `sensitive_patterns`（機密判定の和集合・既定は外せない＝U2b-2）。
/// 空 patterns でも既定機密（`.env` 等）は 403 になる（is_sensitive_with が常に既定を内包＝弱めない）。
fn serve_verified_image(resolved: &Path, sensitive_patterns: &[String]) -> Response<Vec<u8>> {
    // (1) 機密再判定（多層防御・実体名で判定）。既定 ∪ 設定 sensitive_patterns（既定は外せない＝U2b-2）。
    // local_resource_response と同一の防御段（共通 deny_if_sensitive・403）を共有する。
    if let Some(resp) = deny_if_sensitive(resolved, sensitive_patterns) {
        return resp;
    }

    // (2) ファイル上限を metadata で先に弾く（巨大ファイルは中身を読まない＝固まらない）。
    let too_big = std::fs::metadata(resolved)
        .map(|m| m.len() > MAX_IMAGE_FILE_BYTES)
        .unwrap_or(false);
    if too_big {
        return error_response(StatusCode::PAYLOAD_TOO_LARGE, "ファイルが大きすぎます");
    }

    // (3) 読取り。失敗（不在/権限）は 404（情報漏れを避けて最小化）。
    let Ok(bytes) = std::fs::read(resolved) else {
        return error_response(StatusCode::NOT_FOUND, "読み取り失敗");
    };

    // (4) 暴走ガード（要件2.2）。寸法不明/寸法上限超はデコード前に 413 で弾く（UI が固まらない）。
    if let GuardDecision::Block(_) = check_image_bytes(&bytes, DEFAULT_IMAGE_MAX_PIXELS) {
        return error_response(
            StatusCode::PAYLOAD_TOO_LARGE,
            "暴走ガード: 上限超過のため配信拒否",
        );
    }

    // (5) OK 配信。Content-Type は拡張子由来・nosniff（共通 builder が一元付与）・控えめキャッシュ
    // （同一セッションで繰り返し読む）。応答組立は preview と共通の finish_response へ集約。
    let content_type = guess_content_type(resolved);
    finish_response(
        ok_response_builder(content_type).header(header::CACHE_CONTROL, "max-age=300"),
        bytes,
    )
}

/// `pika-asset://`（実体 `http://pika-asset.localhost/`）の custom protocol ハンドラ（要件12.2・U3）。
///
/// メインWebView 用の信頼画像配信。**この protocol はアプリ全体登録**で権限ゼロ別WebView からも到達しうるが、
/// 隔離の関門は origin に依らず三重:
///   (a) [`AccessControl::verify_read`] の path ゲート（封じ込め）。
///   (b) [`serve_verified_image`] 内の `is_sensitive_with` 再判定（機密拒否・設定パターン和集合・
///       既定は外せない＝U2b-2）。state 未登録時は空 patterns＝既定のみで安全側（弱めない）。
///   (c) プレビュー別WebView の CSP に `pika-asset` を入れない（CSP 面で塞ぐ＝`tauri.conf.json`）。
///
/// ルーティング: URL パス（先頭 `/` を除き percent-decode した絶対パス文字列）を `verify_read` へ通し、
/// 成功した実体パスを [`serve_verified_image`] へ渡すだけ（**自前のパス正規化/許可判定は書かない**）。
pub fn handle_asset_request(
    ctx: UriSchemeContext<'_, Wry>,
    request: Request<Vec<u8>>,
) -> Response<Vec<u8>> {
    let app = ctx.app_handle();
    let Some(access) = app.try_state::<AccessControl>() else {
        return error_response(StatusCode::INTERNAL_SERVER_ERROR, "access state 未登録");
    };

    // URL パス（先頭 `/` を除く）を percent-decode して絶対パス文字列にする。
    let raw_path = request.uri().path();
    let encoded = raw_path.strip_prefix('/').unwrap_or(raw_path);
    let raw = percent_decode_str(encoded).decode_utf8_lossy().into_owned();
    if raw.is_empty() {
        return error_response(StatusCode::BAD_REQUEST, "空のパス");
    }

    // 封じ込め（唯一の関門・自前の正規化/許可判定は書かない＝AccessControl に委ねる）。
    let Ok(resolved) = access.verify_read(&raw) else {
        return error_response(StatusCode::FORBIDDEN, "許可されていないパス");
    };

    // 機密判定の設定 sensitive_patterns（和集合・既定は外せない＝U2b-2）を state から取る。
    // 取得は preview 側と共通の単一源（state 未登録時は空 patterns＝既定のみで安全側・弱めない）。
    let sensitive_patterns = crate::settings_service::sensitive_patterns_of(app);

    serve_verified_image(&resolved, &sensitive_patterns)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::path::PathBuf;

    /// 一時ディレクトリを作る（テスト用・衝突回避に nanos＋連番）。
    /// src-tauri の既存テスト（access.rs/snapshot.rs）の流儀に合わせ tempfile crate は使わない。
    fn temp_dir(tag: &str) -> PathBuf {
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(0);
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let seq = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("pika-asset-{tag}-{nanos}-{seq}"));
        std::fs::create_dir_all(&dir).expect("一時ディレクトリ作成");
        dir
    }

    /// 指定ディレクトリにバイト列を書いて canonicalize 済み実体パスを返す。
    fn write_bytes(dir: &Path, name: &str, body: &[u8]) -> PathBuf {
        let p = dir.join(name);
        let mut f = std::fs::File::create(&p).expect("ファイル作成");
        f.write_all(body).expect("書込");
        std::fs::canonicalize(&p).expect("canonicalize")
    }

    /// PNG ヘッダ（シグネチャ + IHDR の width/height）を組み立てる（guard テストと同方式）。
    fn png_header(w: u32, h: u32) -> Vec<u8> {
        let mut b = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
        b.extend_from_slice(&[0, 0, 0, 13]); // IHDR 長
        b.extend_from_slice(b"IHDR");
        b.extend_from_slice(&w.to_be_bytes());
        b.extend_from_slice(&h.to_be_bytes());
        b.extend_from_slice(&[8, 2, 0, 0, 0]); // bit depth 等（残り）
        b
    }

    #[test]
    fn read_prefix_先頭プレフィックスでも正常pngはimageになる() {
        // 寸法マーカーはヘッダ先頭にあるため、後続に大量バイトがあってもプレフィックス読みで Image 判定できる。
        // 判定側がファイルを全読みしないこと（プレフィックス長以下しか読まない）も確認する（指摘2）。
        let dir = temp_dir("prefix");
        let mut body = png_header(640, 480);
        body.extend(std::iter::repeat_n(0u8, 200_000)); // プレフィックス超のダミー後続バイト。
        let path = write_bytes(&dir, "wide.png", &body);
        let prefix = read_prefix(&path, IMAGE_HEADER_PREFIX_BYTES).expect("プレフィックス読取");
        assert!(
            (prefix.len() as u64) <= IMAGE_HEADER_PREFIX_BYTES,
            "プレフィックスが上限を超えて読まれている（全読みになっている）: {} bytes",
            prefix.len()
        );
        assert_eq!(
            image_info_for(&path, &prefix),
            ImageInfoDto::Image {
                width: 640,
                height: 480,
                mime: "image/png".to_string(),
            },
            "プレフィックス読みで正常 PNG が Image にならない"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn image_info_for_正常pngはimageを返す() {
        // 小さな PNG（800x600 = 48万px・上限内）は Image{width,height,mime} になる。
        let png = png_header(800, 600);
        let info = image_info_for(Path::new("a.png"), &png);
        assert_eq!(
            info,
            ImageInfoDto::Image {
                width: 800,
                height: 600,
                mime: "image/png".to_string(),
            },
            "正常 PNG が Image にならない: {info:?}"
        );
    }

    #[test]
    fn image_info_for_寸法上限超はtoo_largeを返す() {
        // 10000x7000 = 7000万px > 6000万px。寸法上限超は TooLarge（外部誘導）。
        let png = png_header(10_000, 7_000);
        let info = image_info_for(Path::new("big.png"), &png);
        assert_eq!(
            info,
            ImageInfoDto::TooLarge { pixels: 70_000_000 },
            "寸法上限超 PNG が TooLarge にならない: {info:?}"
        );
    }

    #[test]
    fn image_info_for_画像でないバイトはunsupportedを返す() {
        // 既知画像マジックに当たらない（画像として描けない）→ Unsupported。
        let info = image_info_for(Path::new("note.txt"), b"this is not an image");
        assert_eq!(
            info,
            ImageInfoDto::Unsupported,
            "非画像バイトが Unsupported にならない: {info:?}"
        );
    }

    #[test]
    fn image_info_for_mimeは拡張子由来() {
        // mime は拡張子から推定（guess_content_type と一致）。jpg は image/jpeg。
        // 寸法は PNG ヘッダから読むが、mime 判定は拡張子（resolved のパス）由来であることを確認する。
        let png = png_header(100, 100);
        let info = image_info_for(Path::new("photo.jpg"), &png);
        match info {
            ImageInfoDto::Image { mime, .. } => {
                assert_eq!(mime, "image/jpeg", "mime が拡張子由来でない")
            }
            other => panic!("Image にならない: {other:?}"),
        }
    }

    #[test]
    fn serve_verified_image_正常pngは200とnosniff() {
        let dir = temp_dir("serve-ok");
        let path = write_bytes(&dir, "ok.png", &png_header(800, 600));
        // 空 patterns（既定のみ）で配信＝設定なしの既定挙動を検証する。
        let resp = serve_verified_image(&path, &[]);
        assert_eq!(resp.status(), StatusCode::OK, "正常 PNG が 200 にならない");
        assert_eq!(
            resp.headers().get(header::CONTENT_TYPE).unwrap(),
            "image/png",
            "Content-Type が image/png でない"
        );
        assert_eq!(
            resp.headers().get("X-Content-Type-Options").unwrap(),
            "nosniff",
            "nosniff が無い"
        );
        assert!(!resp.body().is_empty(), "本体が空");
    }

    #[test]
    fn serve_verified_image_機密名はforbidden() {
        // `.env`（機密）は is_sensitive 再判定で 403。中身が正常 PNG でも配信しない。
        let dir = temp_dir("serve-secret");
        let path = write_bytes(&dir, ".env", &png_header(100, 100));
        // 不変条件: 空 patterns（設定に .env を入れていない）でも .env は既定機密で 403（外せない）。
        let resp = serve_verified_image(&path, &[]);
        assert_eq!(
            resp.status(),
            StatusCode::FORBIDDEN,
            "機密名ファイルが配信拒否されない"
        );
        assert!(resp.body().is_empty(), "機密拒否で本体が漏れた");
    }

    #[test]
    fn serve_verified_image_設定パターン該当は403_和集合で足せる() {
        // U2b-2: 既定外の通常名（normal.png）でも、設定 sensitive_patterns に `*.png` を足すと
        // 機密扱いで 403 になる（和集合＝設定は足すだけ）。中身が正常 PNG でも配信しない。
        let dir = temp_dir("serve-userpat");
        let path = write_bytes(&dir, "normal.png", &png_header(100, 100));
        let patterns = vec!["*.png".to_string()];
        let resp = serve_verified_image(&path, &patterns);
        assert_eq!(
            resp.status(),
            StatusCode::FORBIDDEN,
            "設定パターン該当ファイルが配信拒否されない（和集合で足せない）"
        );
        assert!(resp.body().is_empty(), "設定機密拒否で本体が漏れた");
        // 同じファイルでも空 patterns（設定なし）なら 200 で配信される（パターンは足すだけ）。
        let resp_default = serve_verified_image(&path, &[]);
        assert_eq!(
            resp_default.status(),
            StatusCode::OK,
            "設定パターンなしでは通常 PNG は配信される（既定挙動）"
        );
    }

    #[test]
    fn serve_verified_image_ファイル上限超は413で中身を読まず弾く() {
        // MAX_IMAGE_FILE_BYTES 超のファイルは metadata で 413（中身を読まない＝固まらない）。
        // 64MiB を超える実ファイルを書くのは重いので、先頭は壊れた（非画像）バイトにしておき、
        // 「中身を読まず弾く」ことを「壊れバイトでも 413（404 でも 413（暴走ガード）でもなく
        // ファイル上限の 413）になる」で確認する。サイズだけで弾くため中身の妥当性に依らない。
        let dir = temp_dir("serve-toobig");
        let p = dir.join("huge.png");
        {
            let f = std::fs::File::create(&p).expect("ファイル作成");
            // スパースに 64MiB + 1 バイトへ伸ばす（実データは書かない＝中身を読まない検証の意図）。
            f.set_len(MAX_IMAGE_FILE_BYTES + 1).expect("サイズ設定");
        }
        let path = std::fs::canonicalize(&p).expect("canonicalize");
        let resp = serve_verified_image(&path, &[]);
        assert_eq!(
            resp.status(),
            StatusCode::PAYLOAD_TOO_LARGE,
            "ファイル上限超が 413 にならない"
        );
    }

    #[test]
    fn serve_verified_image_寸法上限超pngは413() {
        // 寸法上限超（10000x7000=7000万px）の PNG ヘッダは暴走ガードで 413。
        let dir = temp_dir("serve-bigdim");
        let path = write_bytes(&dir, "bigdim.png", &png_header(10_000, 7_000));
        let resp = serve_verified_image(&path, &[]);
        assert_eq!(
            resp.status(),
            StatusCode::PAYLOAD_TOO_LARGE,
            "寸法上限超 PNG が 413 にならない"
        );
    }

    #[test]
    fn serve_verified_image_寸法不明バイトは413_fail_closed() {
        // 既知画像マジックに当たらないバイトは check_image_bytes が寸法不明で Block → 413（fail-closed）。
        let dir = temp_dir("serve-unknown");
        let path = write_bytes(&dir, "broken.png", b"not a real image at all");
        let resp = serve_verified_image(&path, &[]);
        assert_eq!(
            resp.status(),
            StatusCode::PAYLOAD_TOO_LARGE,
            "寸法不明バイトが 413（fail-closed）にならない"
        );
    }
}
