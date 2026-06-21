//! ベースライン保存方針の判定（機密ファイル・10MB境界・要件9.1/9.2・design doc 11章）。
//!
//! ベースライン対象（要件9.2 を全文書の正とする）:
//! - 除外リスト外かつ **10MB 未満のテキストファイル**は内容を保存する（差分・巻き戻し可能）。
//! - **10MB 以上**のテキストファイルと画像は **ハッシュのみ**記録し未読判定にのみ使う（差分・巻き戻し非対象）。
//! - **機密ファイル**（既定 `.env`・`*.key`・`*.pem`・`*secret*` 等）は内容を保存せず **ハッシュのみ**
//!   （元ファイル削除後に平文コピーを残さないデータ最小化＝要件9.1）。
//!
//! 「ちょうど 10MB はハッシュのみ」（境界＝10MB 未満のみ内容保存）。
//! 第1段階閾値（10MB）は sprint 6 の CM6 実測で確定する暫定値（spec 補完判断4・要件2.2 TBD）。

/// 内容保存の境界（バイト）。これ**未満**のみ内容を保存し、ちょうど・超過はハッシュのみ。
/// （要件9.2「10MB未満のみ内容保存」。暫定値＝sprint6 CM6 実測で確定）。
pub const DEFAULT_CONTENT_LIMIT_BYTES: u64 = 10 * 1024 * 1024;

/// ベースライン保存方針（観測可能にしてテストで分岐を検証する）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BaselinePolicy {
    /// 内容を object として保存する（差分・巻き戻し可能）。
    StoreContent,
    /// ハッシュのみ記録（差分・巻き戻し非対象）。機密/10MB以上/画像。
    HashOnly,
}

impl BaselinePolicy {
    /// 内容を保存する方針か（差分・巻き戻しが可能か）。
    pub fn stores_content(self) -> bool {
        matches!(self, BaselinePolicy::StoreContent)
    }
}

/// 機密ファイル既定パターンに一致するか（要件9.1 の既定。settings で調整可だが本層は既定を判定）。
///
/// パスのファイル名部分（小文字化）で判定する:
/// - 拡張子 `.key` / `.pem`
/// - ファイル名 `.env`（および `.env.*`）
/// - ファイル名に `secret` を含む
pub fn is_sensitive(path: &str) -> bool {
    let name = file_name_lower(path);
    if name.ends_with(".key") || name.ends_with(".pem") {
        return true;
    }
    if name == ".env" || name.starts_with(".env.") {
        return true;
    }
    name.contains("secret")
}

/// 画像など内容を保存しない非テキスト種別か（拡張子で簡易判定）。
///
/// 画像はハッシュのみ記録（差分・巻き戻し非対象＝要件9.2）。網羅でなく代表的拡張子を判定する。
pub fn is_image(path: &str) -> bool {
    let name = file_name_lower(path);
    const IMAGE_EXTS: &[&str] = &[
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".ico", ".tiff", ".tif",
    ];
    IMAGE_EXTS.iter().any(|ext| name.ends_with(ext))
}

/// パスのファイル名部分を小文字化して返す（`/` と `\` の両区切りに対応）。
fn file_name_lower(path: &str) -> String {
    let after_slash = path.rsplit(['/', '\\']).next().unwrap_or(path);
    after_slash.to_ascii_lowercase()
}

/// ベースライン保存方針を判定する（要件9.1/9.2）。
///
/// - `path`: 対象ファイルパス（機密/画像の拡張子判定に使う）。
/// - `size_bytes`: 内容のバイトサイズ（境界判定に使う）。
///
/// 判定順（いずれかに該当すればハッシュのみ）:
/// 1. 機密ファイル（平文を残さない＝最優先のデータ最小化）。
/// 2. 画像（テキストでない）。
/// 3. サイズが内容保存境界（既定10MB）**以上**（ちょうど10MBもハッシュのみ）。
/// それ以外（除外リスト外の 10MB 未満テキスト）は内容を保存する。
pub fn baseline_policy(path: &str, size_bytes: u64) -> BaselinePolicy {
    if is_sensitive(path) || is_image(path) || size_bytes >= DEFAULT_CONTENT_LIMIT_BYTES {
        BaselinePolicy::HashOnly
    } else {
        BaselinePolicy::StoreContent
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 機密ファイルはハッシュのみ() {
        assert_eq!(baseline_policy(".env", 10), BaselinePolicy::HashOnly);
        assert_eq!(
            baseline_policy("config/.env.local", 10),
            BaselinePolicy::HashOnly
        );
        assert_eq!(
            baseline_policy(r"C:\proj\app.key", 10),
            BaselinePolicy::HashOnly
        );
        assert_eq!(
            baseline_policy("certs/server.pem", 10),
            BaselinePolicy::HashOnly
        );
        assert_eq!(
            baseline_policy("my-secret-notes.txt", 10),
            BaselinePolicy::HashOnly
        );
    }

    #[test]
    fn 画像はハッシュのみ() {
        assert_eq!(baseline_policy("logo.PNG", 100), BaselinePolicy::HashOnly);
        assert_eq!(baseline_policy("photo.jpeg", 100), BaselinePolicy::HashOnly);
    }

    #[test]
    fn ちょうど_10mb_はハッシュのみ() {
        // 境界＝10MB 未満のみ内容保存（要件9.2）。ちょうど 10MB はハッシュのみ。
        assert_eq!(
            baseline_policy("big.md", DEFAULT_CONTENT_LIMIT_BYTES),
            BaselinePolicy::HashOnly
        );
    }

    #[test]
    fn _10mb_未満のテキストは内容保存() {
        assert_eq!(
            baseline_policy("notes.md", DEFAULT_CONTENT_LIMIT_BYTES - 1),
            BaselinePolicy::StoreContent
        );
        assert!(baseline_policy("notes.md", 1024).stores_content());
    }

    #[test]
    fn _10mb_超のテキストはハッシュのみ() {
        assert_eq!(
            baseline_policy("huge.json", DEFAULT_CONTENT_LIMIT_BYTES + 1),
            BaselinePolicy::HashOnly
        );
    }

    #[test]
    fn 通常テキスト名は機密でも画像でもない() {
        assert!(!is_sensitive("readme.md"));
        assert!(!is_image("readme.md"));
    }
}
