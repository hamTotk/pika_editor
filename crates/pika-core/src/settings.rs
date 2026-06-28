//! settings.toml の解析（純粋ロジック・要件10.3/10.4・cargo test の決定論ゲート対象）。
//!
//! 役割（design doc 3章「コアは UI を知らない」）:
//! - settings.toml 本文（`&str`）を受け取り、`Settings` へ写す純粋関数だけを置く。
//! - ファイル I/O・監視・emit は src-tauri::settings_service（薄い橋渡し層）が担う。
//!
//! 解析方針（要件10.3/10.4 の3挙動を成立させる核）:
//! - **per-field フォールバック**: TOML 全体が parse できれば、各キーを `toml::Table` から手動抽出する。
//!   型/値域が不正なキーは既定へフォールバックし、警告（キー名）を積む。serde derive は使わない
//!   ——serde は 1 フィールドの不正で全体 deserialize が失敗し「壊れた1キー以外は既定で起動」を
//!   実現できないため（要件10.3「壊れていても既定値で起動」とは別問題＝部分不正の救済）。
//! - **TOML 自体の parse 失敗**（保存途中の不完全 TOML 等）は [`SettingsLoad::KeepPrevious`] を返し、
//!   呼び出し側が「直前の有効設定を維持」（要件10.4）か「起動時は既定」（要件10.3）かを選べるようにする。
//!
//! settings.toml の置き場はデータルート配下（`<data_root>/settings.toml`・既存 state.json と同じ）。
//! 要件文の %APPDATA% 記述は doc 不整合で、実装は一貫して data_root を使う（src-tauri 側で解決）。

/// テーマ設定（要件11.3・ui-design 配色トークン）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ThemeSetting {
    /// 明色固定。
    Light,
    /// 暗色固定。
    Dark,
    /// OS のテーマに追従（既定）。
    System,
}

/// 起動時の既定表示モード（要件6.1・ui-design 8章）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DefaultMode {
    /// ソース（編集）表示で開く（既定）。
    Source,
    /// プレビュー表示で開く。
    Preview,
}

/// settings.toml で表現する設定一式（要件10.3/10.4）。
///
/// 既定値は各サブシステムの既存定数と一致させる（watcher の除外ディレクトリ・巨大ファイル閾値等）。
/// 個別設定値を各挙動へ配線するのは**段階的**（本バッチは機構の貫通＋取得 API まで。適用は後続）。
#[derive(Debug, Clone, PartialEq)]
pub struct Settings {
    /// ツリー/監視の除外ディレクトリ（既定 `[".git","node_modules"]`・watcher EXCLUDED_DIRS と一致）。
    pub excluded_dirs: Vec<String>,
    /// 巨大ファイル閾値バイト（既定 10MiB・hashing::HUGE_FILE_THRESHOLD_BYTES と一致）。
    pub huge_file_threshold_bytes: u64,
    /// 長大行の折返し抑制しきい値（文字数・既定 100_000）。
    pub long_line_chars: usize,
    /// 機密ファイルのパターン（ハッシュのみ記録＋配信拒否・既定 `[".env","*.key","*.pem","*secret*"]`）。
    pub sensitive_patterns: Vec<String>,
    /// プレビューの外部リソース取得を許可するか（既定 false＝オプトイン）。
    pub allow_remote_resources: bool,
    /// 折返しの既定 ON/OFF（既定 true）。
    pub wrap_default: bool,
    /// タブ幅（既定 4）。
    pub tab_width: u8,
    /// テーマ（既定 System）。
    pub theme: ThemeSetting,
    /// 起動時に全ファイルの内容ハッシュを取り未読照合するか（既定 false・unread.fullHashOnStartup）。
    pub full_hash_on_startup: bool,
    /// Mermaid 図の描画を有効化するか（既定 true）。
    pub feature_mermaid: bool,
    /// 数式（KaTeX）の描画を有効化するか（既定 true）。
    pub feature_math: bool,
    /// シンタックスハイライトを有効化するか（既定 true）。
    pub feature_highlight: bool,
    /// 起動時の既定表示モード（既定 Source）。
    pub default_mode: DefaultMode,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            excluded_dirs: vec![".git".to_string(), "node_modules".to_string()],
            huge_file_threshold_bytes: 10 * 1024 * 1024,
            long_line_chars: 100_000,
            sensitive_patterns: vec![
                ".env".to_string(),
                "*.key".to_string(),
                "*.pem".to_string(),
                "*secret*".to_string(),
            ],
            allow_remote_resources: false,
            wrap_default: true,
            tab_width: 4,
            theme: ThemeSetting::System,
            full_hash_on_startup: false,
            feature_mermaid: true,
            feature_math: true,
            feature_highlight: true,
            default_mode: DefaultMode::Source,
        }
    }
}

/// settings.toml の読み込み結果（要件10.3/10.4 の3挙動を分岐で表現する）。
#[derive(Debug, Clone, PartialEq)]
pub enum SettingsLoad {
    /// TOML 全体の parse に成功した。無効値のキーは既定へフォールバックし、`warnings` に該当キー名を積む。
    ///
    /// - 起動時（要件10.3）: 既定起動＋（warnings 非空なら）警告。
    /// - 実行中（要件10.4）: current を更新し（warnings 非空なら）警告。
    Loaded {
        /// 解決済み設定（無効値は既定でフォールバック済み）。
        settings: Settings,
        /// 既定へフォールバックした無効値のキー名（空なら警告なし）。
        warnings: Vec<String>,
    },
    /// TOML 自体の parse に失敗した（保存途中の不完全 TOML 等）。
    ///
    /// 呼び出し側は「直前の有効設定を維持」（要件10.4・実行中）すべき。直前が無い起動時（要件10.3）は
    /// 既定へフォールバックし警告を出す（判断は src-tauri 側）。
    KeepPrevious {
        /// parse 失敗の理由（診断/通知文の補足用）。
        reason: String,
    },
}

/// settings.toml 本文を解析する（I/O 非依存・要件10.3/10.4）。
///
/// - 空文字（初回/空ファイル）→ `Loaded { Settings::default(), warnings: [] }`（既定・警告なし）。
/// - TOML parse 失敗 → `KeepPrevious`（不完全 TOML＝直前維持すべき）。
/// - parse 成功 → 各キーを手動抽出。型/値域不正は既定へフォールバックし warnings へキー名を積む。
///   キー不在は正常（部分指定）なので warnings に積まない。
pub fn load_settings(toml_text: &str) -> SettingsLoad {
    // 空ファイル/未作成は既定（初回起動の正常系・警告を出さない）。
    if toml_text.trim().is_empty() {
        return SettingsLoad::Loaded {
            settings: Settings::default(),
            warnings: Vec::new(),
        };
    }

    // TOML 全体の parse。失敗＝保存途中の不完全 TOML 等 → 直前維持（要件10.4）。
    let table: toml::Table = match toml_text.parse::<toml::Table>() {
        Ok(t) => t,
        Err(e) => {
            return SettingsLoad::KeepPrevious {
                reason: e.to_string(),
            }
        }
    };

    let mut s = Settings::default();
    let mut warnings: Vec<String> = Vec::new();

    // 真偽値の per-field 抽出（型不正なら既定維持＋警告）。
    extract_bool(
        &table,
        "allow_remote_resources",
        &mut s.allow_remote_resources,
        &mut warnings,
    );
    extract_bool(&table, "wrap_default", &mut s.wrap_default, &mut warnings);
    extract_bool(
        &table,
        "full_hash_on_startup",
        &mut s.full_hash_on_startup,
        &mut warnings,
    );
    extract_bool(
        &table,
        "feature_mermaid",
        &mut s.feature_mermaid,
        &mut warnings,
    );
    extract_bool(&table, "feature_math", &mut s.feature_math, &mut warnings);
    extract_bool(
        &table,
        "feature_highlight",
        &mut s.feature_highlight,
        &mut warnings,
    );

    // 整数の per-field 抽出（型/値域不正なら既定維持＋警告）。スロット型ごとの値域は
    // `TryFrom<i64>` が担う（u64/usize は負値で失敗・u8 は 0..=255 以外で失敗＝従来の手書き範囲と等価）。
    extract_int(
        &table,
        "huge_file_threshold_bytes",
        &mut s.huge_file_threshold_bytes,
        &mut warnings,
    );
    extract_int(
        &table,
        "long_line_chars",
        &mut s.long_line_chars,
        &mut warnings,
    );
    extract_int(&table, "tab_width", &mut s.tab_width, &mut warnings);

    // 文字列配列の per-field 抽出（配列でない/要素が文字列でないなら既定維持＋警告）。
    extract_string_array(&table, "excluded_dirs", &mut s.excluded_dirs, &mut warnings);
    extract_string_array(
        &table,
        "sensitive_patterns",
        &mut s.sensitive_patterns,
        &mut warnings,
    );

    // 列挙の per-field 抽出（既知の文字列以外は既定維持＋警告）。
    extract_theme(&table, &mut s.theme, &mut warnings);
    extract_default_mode(&table, &mut s.default_mode, &mut warnings);

    SettingsLoad::Loaded {
        settings: s,
        warnings,
    }
}

/// 真偽値を抽出する。キー不在は何もしない（部分指定＝正常）。型不正は既定維持＋警告。
fn extract_bool(table: &toml::Table, key: &str, slot: &mut bool, warnings: &mut Vec<String>) {
    match table.get(key) {
        None => {}
        Some(v) => match v.as_bool() {
            Some(b) => *slot = b,
            None => warnings.push(key.to_string()),
        },
    }
}

/// 整数を抽出する（u64/usize/u8 共通・型/値域不正は既定維持＋警告）。
///
/// TOML 整数は i64。スロット型 `T` の `TryFrom<i64>` が値域検査を兼ねる:
/// - `u64`/`usize`: 負値で失敗（従来の `i >= 0` ガードと等価。x64 では usize==u64 で範囲も一致）。
/// - `u8`: 0..=255 以外で失敗（従来の `(0..=255).contains(&i)` と等価）。
///
/// キー不在は何もしない（部分指定＝正常）。型不正（非整数）や値域外は既定維持＋警告。
fn extract_int<T: TryFrom<i64>>(
    table: &toml::Table,
    key: &str,
    slot: &mut T,
    warnings: &mut Vec<String>,
) {
    let Some(v) = table.get(key) else { return };
    match v.as_integer().and_then(|i| T::try_from(i).ok()) {
        Some(n) => *slot = n,
        None => warnings.push(key.to_string()),
    }
}

/// 文字列配列を抽出する。配列でない/要素に文字列以外を含むなら既定維持＋警告（全採用 or 全棄却）。
fn extract_string_array(
    table: &toml::Table,
    key: &str,
    slot: &mut Vec<String>,
    warnings: &mut Vec<String>,
) {
    let Some(v) = table.get(key) else { return };
    let Some(arr) = v.as_array() else {
        warnings.push(key.to_string());
        return;
    };
    let mut out = Vec::with_capacity(arr.len());
    for item in arr {
        match item.as_str() {
            Some(s) => out.push(s.to_string()),
            None => {
                // 1 要素でも非文字列が混ざれば配列全体を不正扱い（部分採用で予期せぬ挙動を避ける）。
                warnings.push(key.to_string());
                return;
            }
        }
    }
    *slot = out;
}

/// theme を抽出する（"light"/"dark"/"system"）。未知文字列/型不正は既定維持＋警告。
fn extract_theme(table: &toml::Table, slot: &mut ThemeSetting, warnings: &mut Vec<String>) {
    let Some(v) = table.get("theme") else { return };
    match v.as_str() {
        Some("light") => *slot = ThemeSetting::Light,
        Some("dark") => *slot = ThemeSetting::Dark,
        Some("system") => *slot = ThemeSetting::System,
        _ => warnings.push("theme".to_string()),
    }
}

/// default_mode を抽出する（"source"/"preview"）。未知文字列/型不正は既定維持＋警告。
fn extract_default_mode(table: &toml::Table, slot: &mut DefaultMode, warnings: &mut Vec<String>) {
    let Some(v) = table.get("default_mode") else {
        return;
    };
    match v.as_str() {
        Some("source") => *slot = DefaultMode::Source,
        Some("preview") => *slot = DefaultMode::Preview,
        _ => warnings.push("default_mode".to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 空文字は既定で警告なし() {
        match load_settings("") {
            SettingsLoad::Loaded { settings, warnings } => {
                assert_eq!(settings, Settings::default());
                assert!(warnings.is_empty());
            }
            other => panic!("Loaded を期待: {other:?}"),
        }
    }

    #[test]
    fn 空白のみも既定で警告なし() {
        match load_settings("   \n\t  \n") {
            SettingsLoad::Loaded { settings, warnings } => {
                assert_eq!(settings, Settings::default());
                assert!(warnings.is_empty());
            }
            other => panic!("Loaded を期待: {other:?}"),
        }
    }

    #[test]
    fn 完全な正当_toml_は全フィールド反映_警告なし() {
        let text = r#"
            excluded_dirs = [".git", "target", "dist"]
            huge_file_threshold_bytes = 20971520
            long_line_chars = 50000
            sensitive_patterns = ["*.token"]
            allow_remote_resources = true
            wrap_default = false
            tab_width = 2
            theme = "dark"
            full_hash_on_startup = true
            feature_mermaid = false
            feature_math = false
            feature_highlight = false
            default_mode = "preview"
        "#;
        match load_settings(text) {
            SettingsLoad::Loaded { settings, warnings } => {
                assert!(warnings.is_empty(), "warnings: {warnings:?}");
                assert_eq!(
                    settings.excluded_dirs,
                    vec![".git".to_string(), "target".to_string(), "dist".to_string()]
                );
                assert_eq!(settings.huge_file_threshold_bytes, 20_971_520);
                assert_eq!(settings.long_line_chars, 50_000);
                assert_eq!(settings.sensitive_patterns, vec!["*.token".to_string()]);
                assert!(settings.allow_remote_resources);
                assert!(!settings.wrap_default);
                assert_eq!(settings.tab_width, 2);
                assert_eq!(settings.theme, ThemeSetting::Dark);
                assert!(settings.full_hash_on_startup);
                assert!(!settings.feature_mermaid);
                assert!(!settings.feature_math);
                assert!(!settings.feature_highlight);
                assert_eq!(settings.default_mode, DefaultMode::Preview);
            }
            other => panic!("Loaded を期待: {other:?}"),
        }
    }

    #[test]
    fn 部分指定は指定キーのみ反映_他は既定_警告なし() {
        match load_settings("tab_width = 8\n") {
            SettingsLoad::Loaded { settings, warnings } => {
                assert!(warnings.is_empty());
                assert_eq!(settings.tab_width, 8);
                // 他は既定のまま。
                assert_eq!(settings.theme, ThemeSetting::System);
                assert!(settings.wrap_default);
                assert_eq!(settings.excluded_dirs, Settings::default().excluded_dirs);
            }
            other => panic!("Loaded を期待: {other:?}"),
        }
    }

    #[test]
    fn 不正値は該当キーが既定にフォールバックし警告に積まれる() {
        // tab_width が文字列・theme が未知・huge_file_threshold_bytes が負（型/値域不正）。
        let text = r#"
            tab_width = "abc"
            theme = "purple"
            huge_file_threshold_bytes = -1
        "#;
        match load_settings(text) {
            SettingsLoad::Loaded { settings, warnings } => {
                assert_eq!(settings.tab_width, 4);
                assert_eq!(settings.theme, ThemeSetting::System);
                assert_eq!(settings.huge_file_threshold_bytes, 10 * 1024 * 1024);
                assert!(warnings.contains(&"tab_width".to_string()));
                assert!(warnings.contains(&"theme".to_string()));
                assert!(warnings.contains(&"huge_file_threshold_bytes".to_string()));
                assert_eq!(warnings.len(), 3);
            }
            other => panic!("Loaded を期待: {other:?}"),
        }
    }

    #[test]
    fn 壊れた_toml_は_keep_previous() {
        // 値の無い代入＝TOML として不正（保存途中の不完全 TOML 相当）。
        match load_settings("foo =\n") {
            SettingsLoad::KeepPrevious { reason } => {
                assert!(!reason.is_empty());
            }
            other => panic!("KeepPrevious を期待: {other:?}"),
        }
    }

    #[test]
    fn 不完全な途中保存_toml_は_keep_previous() {
        // 閉じていない配列＝途中までしか書けていない保存の典型。
        match load_settings("excluded_dirs = [\".git\", \n") {
            SettingsLoad::KeepPrevious { .. } => {}
            other => panic!("KeepPrevious を期待: {other:?}"),
        }
    }

    #[test]
    fn 配列キーに非配列を与えると既定_警告() {
        match load_settings("excluded_dirs = \"not-an-array\"\n") {
            SettingsLoad::Loaded { settings, warnings } => {
                assert_eq!(settings.excluded_dirs, Settings::default().excluded_dirs);
                assert!(warnings.contains(&"excluded_dirs".to_string()));
            }
            other => panic!("Loaded を期待: {other:?}"),
        }
    }

    #[test]
    fn 配列に非文字列要素が混ざると既定_警告() {
        match load_settings("sensitive_patterns = [\".env\", 42]\n") {
            SettingsLoad::Loaded { settings, warnings } => {
                assert_eq!(
                    settings.sensitive_patterns,
                    Settings::default().sensitive_patterns
                );
                assert!(warnings.contains(&"sensitive_patterns".to_string()));
            }
            other => panic!("Loaded を期待: {other:?}"),
        }
    }

    #[test]
    fn theme_各値が正しく列挙へ写る() {
        for (text, expected) in [
            ("theme = \"light\"\n", ThemeSetting::Light),
            ("theme = \"dark\"\n", ThemeSetting::Dark),
            ("theme = \"system\"\n", ThemeSetting::System),
        ] {
            match load_settings(text) {
                SettingsLoad::Loaded { settings, warnings } => {
                    assert_eq!(settings.theme, expected);
                    assert!(warnings.is_empty());
                }
                other => panic!("Loaded を期待: {other:?}"),
            }
        }
    }

    #[test]
    fn default_mode_各値が正しく列挙へ写る() {
        for (text, expected) in [
            ("default_mode = \"source\"\n", DefaultMode::Source),
            ("default_mode = \"preview\"\n", DefaultMode::Preview),
        ] {
            match load_settings(text) {
                SettingsLoad::Loaded { settings, warnings } => {
                    assert_eq!(settings.default_mode, expected);
                    assert!(warnings.is_empty());
                }
                other => panic!("Loaded を期待: {other:?}"),
            }
        }
    }

    #[test]
    fn tab_width_255超は値域不正で既定_警告() {
        match load_settings("tab_width = 300\n") {
            SettingsLoad::Loaded { settings, warnings } => {
                assert_eq!(settings.tab_width, 4);
                assert!(warnings.contains(&"tab_width".to_string()));
            }
            other => panic!("Loaded を期待: {other:?}"),
        }
    }
}
