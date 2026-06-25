//! ベースライン保存方針の判定（機密ファイル・10MB境界・要件9.1/9.2・design doc 11章）。
//!
//! ベースライン対象（要件9.2 を全文書の正とする）:
//! - 除外リスト外かつ **10MB 未満のテキストファイル**は内容を保存する（差分・巻き戻し可能）。
//! - **10MB 以上**のテキストファイルと画像は **ハッシュのみ**記録し未読判定にのみ使う（差分・巻き戻し非対象）。
//! - **機密ファイル**（既定 `.env`・`*.key`・`*.pem`・`*secret*` 等）は内容を保存せず **ハッシュのみ**
//!   （元ファイル削除後に平文コピーを残さないデータ最小化＝要件9.1）。
//!
//! 「ちょうど 10MB はハッシュのみ」（境界＝10MB 未満のみ内容保存）。
//! 第1段階閾値（10MB）は sprint 6 の CM6 実測（系統C・acceptance-findings.md）で
//! 「10MB で編集・検索・保存が通常通り可能」を確認し**確定（維持）**した（要件2.2/9.2・design doc 16章）。

/// 内容保存の境界（バイト）。これ**未満**のみ内容を保存し、ちょうど・超過はハッシュのみ。
/// （要件9.2「10MB未満のみ内容保存」。sprint6 CM6 実測で 10MB に確定）。
/// [`crate::huge::STAGE1_THRESHOLD_BYTES`]・[`crate::hashing::HUGE_FILE_THRESHOLD_BYTES`] と同値。
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
///
/// 設定の `sensitive_patterns`（和集合）を加味する版は [`is_sensitive_with`]。本関数は
/// `is_sensitive_with(path, &[])` と等価（既定のみ）であり、**既定は設定で外せない**不変条件の
/// 土台になる（`is_sensitive_with` は常に本関数を先に評価する）。
pub fn is_sensitive(path: &str) -> bool {
    is_sensitive_with(path, &[])
}

/// 機密ファイル判定（既定パターン ∪ 設定 `sensitive_patterns`・要件9.1）。
///
/// **不変条件（最重要）**: 既定機密パターン（`.env`/`.env.*`・`.key`/`.pem`・`secret` 含む）は
/// 設定で**外せない**。本関数は常に既定判定を**先に**評価し、true ならそのまま true を返す。
/// その後、設定 `patterns`（glob・足すだけ＝和集合）のいずれかにファイル名が一致すれば true。
/// したがって設定は機密を**足せるが減らせない**（既定を内包する＝設定取得不能＝空 patterns でも安全側）。
///
/// - `path`: 対象ファイルパス（`/`・`\` どちらの区切りでも可。ファイル名部分のみを判定）。
/// - `patterns`: 設定 `sensitive_patterns`（glob・`*`=0文字以上の任意のみ対応）。**小文字化は不要**
///   （本関数が `file_name_lower` 由来の小文字名と、パターンを小文字化してから突合する）。
pub fn is_sensitive_with(path: &str, patterns: &[String]) -> bool {
    let name = file_name_lower(path);
    // --- 既定（外せない）を先に評価する＝設定で減らせない不変条件の土台。 ---
    if name.ends_with(".key") || name.ends_with(".pem") {
        return true;
    }
    if name == ".env" || name.starts_with(".env.") {
        return true;
    }
    if name.contains("secret") {
        return true;
    }
    // --- 設定パターン（足すだけ＝和集合）。glob を小文字化してファイル名と突合する。 ---
    patterns
        .iter()
        .any(|pat| glob_match(&pat.to_ascii_lowercase(), &name))
}

/// 簡易 glob マッチ（`*`=0文字以上の任意のみ対応・`?`/文字クラスは非対応）。
///
/// **ReDoS フリーの根拠**: 古典的な2ポインタ greedy（反復・再帰なし）。`star_idx`/`match_idx` の
/// バックトラックは「最後に出現した `*` まで」の1段のみで、`name`/`pattern` の各位置を高々
/// 線形回数しか進めない（計算量 O(|name|·|pattern|) 上限・指数爆発する経路を持たない）。
/// 正規表現エンジンを使わないため catastrophic backtracking は構造上発生しない。
///
/// `pattern`/`name` は呼び出し側で小文字化済みを渡す前提（大小無視は呼び出し側の責務）。
fn glob_match(pattern: &str, name: &str) -> bool {
    let pat: Vec<char> = pattern.chars().collect();
    let txt: Vec<char> = name.chars().collect();
    let (mut p, mut t) = (0usize, 0usize);
    // 直近に出現した `*` の位置と、その `*` がマッチを開始した text 位置（バックトラック用）。
    let mut star: Option<usize> = None;
    let mut star_t = 0usize;
    while t < txt.len() {
        if p < pat.len() && pat[p] == '*' {
            // `*` は 0 文字以上にマッチ。まず 0 文字でパターンを進め、失敗時に text を1つ食わせる。
            star = Some(p);
            star_t = t;
            p += 1;
        } else if p < pat.len() && pat[p] == txt[t] {
            // 通常文字が一致＝両方進める。
            p += 1;
            t += 1;
        } else if let Some(sp) = star {
            // 不一致だが直近に `*` がある＝`*` に text を1文字食わせて再試行（バックトラックは1段のみ）。
            p = sp + 1;
            star_t += 1;
            t = star_t;
        } else {
            // 不一致で `*` も無い＝マッチ不成立。
            return false;
        }
    }
    // text を消費し切った。残るパターンが全て `*` ならマッチ成立（末尾 `*` の境界）。
    while p < pat.len() && pat[p] == '*' {
        p += 1;
    }
    p == pat.len()
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
///
/// 機密判定は既定パターンのみ（設定非依存）。設定 `sensitive_patterns` を加味する版は
/// [`baseline_policy_with`]。本関数は `baseline_policy_with(path, size, &[])` と等価（既定挙動）。
pub fn baseline_policy(path: &str, size_bytes: u64) -> BaselinePolicy {
    baseline_policy_with(path, size_bytes, &[])
}

/// ベースライン保存方針を判定する（既定 ∪ 設定 `sensitive_patterns`・要件9.1/9.2）。
///
/// [`baseline_policy`] と判定順は同一だが、機密判定に [`is_sensitive_with`] を使う（設定パターンの
/// 和集合・既定は外せない）。設定該当ファイルは内容を保存せず **HashOnly**（平文を残さない＝要件9.1）。
///
/// - `path`: 対象ファイルパス。
/// - `size_bytes`: 内容のバイトサイズ（境界判定に使う）。
/// - `patterns`: 設定 `sensitive_patterns`（空なら既定のみ＝[`baseline_policy`] と一致）。
pub fn baseline_policy_with(path: &str, size_bytes: u64, patterns: &[String]) -> BaselinePolicy {
    if is_sensitive_with(path, patterns)
        || is_image(path)
        || size_bytes >= DEFAULT_CONTENT_LIMIT_BYTES
    {
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

    // --- U2b-2: glob_match / is_sensitive_with / baseline_policy_with（設定パターン和集合）。 ---

    #[test]
    fn glob_match_アスタリスク前後と完全一致() {
        // `*.key` ↔ `a.key` 一致（末尾固定・先頭ワイルド）。
        assert!(glob_match("*.key", "a.key"));
        // `*secret*` ↔ `my_secret_file` 一致（両端ワイルド・中間固定）。
        assert!(glob_match("*secret*", "my_secret_file"));
        // `.env` ↔ `.env` は完全一致（ワイルドなし）。
        assert!(glob_match(".env", ".env"));
        // `.env` ↔ `.environment` は不一致（完全一致パターンは前方一致でない）。
        assert!(!glob_match(".env", ".environment"));
        // `config.json` ↔ `config.json` 一致・`other.json` 不一致。
        assert!(glob_match("config.json", "config.json"));
        assert!(!glob_match("config.json", "other.json"));
    }

    #[test]
    fn glob_match_アスタリスク単独と境界() {
        // `*` は任意（空文字含む）に一致。
        assert!(glob_match("*", "anything"));
        assert!(glob_match("*", ""));
        // 先頭 `*` の境界: `*.pem` は `server.pem` に一致、`server.key` に不一致。
        assert!(glob_match("*.pem", "server.pem"));
        assert!(!glob_match("*.pem", "server.key"));
        // 末尾 `*` の境界: `id_*` は `id_rsa`/`id_` に一致、`id`（区切り前で終端）に不一致。
        assert!(glob_match("id_*", "id_rsa"));
        assert!(glob_match("id_*", "id_"));
        assert!(!glob_match("id_*", "id"));
        // 連続 `*` も 0 文字以上として正しく潰れる（ReDoS フリー・指数爆発しない）。
        assert!(glob_match("**a**", "xxaxx"));
        assert!(!glob_match("a*b", "axc"));
    }

    #[test]
    fn is_sensitive_with_空patternsでも既定は機密() {
        // 不変条件: patterns が空でも既定（.env/.key/.pem/secret）は機密（is_sensitive で担保）。
        assert!(is_sensitive_with(".env", &[]));
        assert!(is_sensitive_with("a.key", &[]));
        assert!(is_sensitive_with("x.pem", &[]));
        assert!(is_sensitive_with("mysecret.txt", &[]));
        // 既定に当たらない通常名は空 patterns では非機密。
        assert!(!is_sensitive_with("readme.md", &[]));
    }

    #[test]
    fn is_sensitive_with_ユーザー追加パターンで機密になる() {
        // ユーザーが `*.token` を足すと `api.token` が機密になる（和集合＝足すだけ）。
        let patterns = vec!["*.token".to_string()];
        assert!(is_sensitive_with("api.token", &patterns));
        // パターンに当たらない通常名は非機密のまま。
        assert!(!is_sensitive_with("api.json", &patterns));
        // 設定パターンも大小無視（glob は小文字化して突合する）。
        assert!(is_sensitive_with("API.TOKEN", &patterns));
    }

    #[test]
    fn is_sensitive_with_既定は設定で外せない不変条件() {
        // patterns に `.env` 相当を一切入れなくても（むしろ無関係なパターンしか入れなくても）、
        // `.env` は依然機密（is_sensitive を先に評価＝減らせない不変条件）。
        let patterns = vec!["*.unrelated".to_string()];
        assert!(
            is_sensitive_with(".env", &patterns),
            ".env は設定に依らず常に機密（外せない）"
        );
        assert!(
            is_sensitive_with("server.key", &patterns),
            ".key は設定に依らず常に機密（外せない）"
        );
    }

    #[test]
    fn baseline_policy_with_ユーザーパターン該当はハッシュのみ() {
        // ユーザーパターン該当ファイルは内容を保存しない（HashOnly＝平文を残さない）。
        let patterns = vec!["*.token".to_string()];
        assert_eq!(
            baseline_policy_with("api.token", 10, &patterns),
            BaselinePolicy::HashOnly
        );
        // パターン非該当の通常テキストは内容保存。
        assert_eq!(
            baseline_policy_with("notes.md", 10, &patterns),
            BaselinePolicy::StoreContent
        );
    }

    #[test]
    fn baseline_policy_with_既定パターンは空patternsでもハッシュのみ() {
        // 既定（.env）は空 patterns でも HashOnly（既存 baseline_policy と一致＝回帰ゼロ）。
        assert_eq!(
            baseline_policy_with(".env", 10, &[]),
            BaselinePolicy::HashOnly
        );
        assert_eq!(baseline_policy(".env", 10), BaselinePolicy::HashOnly);
        // 既定挙動の等価性（baseline_policy は baseline_policy_with(.., &[]) へ委譲）。
        assert_eq!(
            baseline_policy("notes.md", 1024),
            baseline_policy_with("notes.md", 1024, &[])
        );
    }
}
