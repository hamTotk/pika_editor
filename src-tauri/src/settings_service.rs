//! settings.toml の監視反映の FS 配線（要件10.3/10.4・design doc 3章「薄い橋渡し層」）。
//!
//! 役割:
//! - 解析の純粋ロジック（per-field フォールバック・KeepPrevious 判定）は **pika-core::settings**
//!   （cargo test 済み）に委ねる。
//! - ここには「データルート配下の settings.toml を read → core の `load_settings` を呼ぶ →
//!   結果に応じて current を更新し frontend へ emit する」の I/O とスレッド配線のみを置く。
//!
//! 要件10.3/10.4 の3挙動:
//! 1. **再起動なしで反映**: [`SettingsService::spawn_watch`] が mtime を 2 秒間隔でポーリングし、
//!    変化したら再パース→`emit('settings-changed')`（[`spawn_watch`] の Loaded 分岐）。
//! 2. **起動時破損→既定＋警告**: [`SettingsService::load_initial`] が `KeepPrevious` を受けたら
//!    既定で起動し `emit('settings-warning')`（起動時は直前の有効設定が無いため既定へ倒す）。
//! 3. **実行中の不完全保存→直前維持＋警告**: [`spawn_watch`] が `KeepPrevious` を受けたら current を
//!    据え置き `emit('settings-warning')`（既定への全戻しでちらつかせない）。
//!
//! 監視方式: 文書監視（WatcherService・workspace 配下）とは別系統で、data_root 直下の単一ファイルを
//! mtime ポーリングで見る。notify の単一ファイル癖/保存途中の中間状態ノイズを避け、間隔でデバウンスを
//! 担保しつつアトミックに read する（固まらない＝専用スレッド・UI を 200ms 超ブロックしない）。

use pika_core::settings::{load_settings, DefaultMode, Settings, SettingsLoad, ThemeSetting};
use serde::Serialize;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, UNIX_EPOCH};
use tauri::{AppHandle, Emitter, Manager, State};

/// settings.toml のポーリング間隔（要件10.3 再起動なし反映・data_root 単一ファイルなので 2 秒で十分）。
const POLL_INTERVAL: Duration = Duration::from_secs(2);

/// frontend へ降ろす設定 DTO（'settings-changed' と get_settings command で使う・enum は文字列へ）。
#[derive(Debug, Clone, Serialize)]
pub struct SettingsDto {
    pub excluded_dirs: Vec<String>,
    pub huge_file_threshold_bytes: u64,
    pub long_line_chars: usize,
    pub sensitive_patterns: Vec<String>,
    pub allow_remote_resources: bool,
    pub wrap_default: bool,
    pub tab_width: u8,
    /// "light" | "dark" | "system"
    pub theme: String,
    pub full_hash_on_startup: bool,
    pub feature_mermaid: bool,
    pub feature_math: bool,
    pub feature_highlight: bool,
    /// "source" | "preview"
    pub default_mode: String,
}

impl From<Settings> for SettingsDto {
    fn from(s: Settings) -> Self {
        SettingsDto {
            excluded_dirs: s.excluded_dirs,
            huge_file_threshold_bytes: s.huge_file_threshold_bytes,
            long_line_chars: s.long_line_chars,
            sensitive_patterns: s.sensitive_patterns,
            allow_remote_resources: s.allow_remote_resources,
            wrap_default: s.wrap_default,
            tab_width: s.tab_width,
            theme: match s.theme {
                ThemeSetting::Light => "light",
                ThemeSetting::Dark => "dark",
                ThemeSetting::System => "system",
            }
            .to_string(),
            full_hash_on_startup: s.full_hash_on_startup,
            feature_mermaid: s.feature_mermaid,
            feature_math: s.feature_math,
            feature_highlight: s.feature_highlight,
            default_mode: match s.default_mode {
                DefaultMode::Source => "source",
                DefaultMode::Preview => "preview",
            }
            .to_string(),
        }
    }
}

/// 設定サービス（managed state）。current（有効設定）を保持し、ポーリングで再読み込みする。
pub struct SettingsService {
    /// 現在の有効設定（起動ロードで上書き・実行中の再パースで更新／直前維持）。
    current: Mutex<Settings>,
    /// 監視対象 `<data_root>/settings.toml` の絶対パス。
    path: PathBuf,
}

impl SettingsService {
    /// サービスを作る（初期 current は既定。`load_initial` で起動時の実値へ上書きする）。
    pub fn new(path: PathBuf) -> Self {
        Self {
            current: Mutex::new(Settings::default()),
            path,
        }
    }

    /// 現在の有効設定の複製を返す（get_settings command が DTO 化して frontend へ返す）。
    pub fn snapshot(&self) -> Settings {
        self.current
            .lock()
            .map(|g| g.clone())
            // ロック毒化時も既定で続行する（設定取得を止めない＝固まらない）。
            .unwrap_or_default()
    }

    /// 起動時に settings.toml を一度読み、current を確定する（要件10.3 挙動②）。
    ///
    /// - ファイル不在/空 → 既定（初回起動の正常系・警告なし）。
    /// - parse 成功・無効値あり → 既定フォールバック済み設定＋無効キーを列挙した警告を emit。
    /// - **parse 失敗（破損）** → 起動時は直前の有効設定が無いため **既定で起動**し、保全を案内する警告を emit
    ///   （要件10.3「壊れていても既定値で起動し警告」）。
    pub fn load_initial(&self, app: &AppHandle) {
        // 不在は空文字扱い（初回＝既定・警告なし）。read 失敗（権限等）も同様に既定で続行する。
        let text = std::fs::read_to_string(&self.path).unwrap_or_default();
        match load_settings(&text) {
            SettingsLoad::Loaded { settings, warnings } => {
                if let Ok(mut g) = self.current.lock() {
                    *g = settings;
                }
                if !warnings.is_empty() {
                    emit_invalid_keys_warning(app, &warnings);
                }
            }
            SettingsLoad::KeepPrevious { .. } => {
                // 起動時は直前が無い → 既定で起動（current は new 時点で既定のまま）。元ファイルは保全する。
                let _ = app.emit(
                    "settings-warning",
                    "設定ファイルが壊れているため既定値で起動しました（元ファイルは保全されています）"
                        .to_string(),
                );
            }
        }
    }

    /// settings.toml のポーリング監視スレッドを起動する（要件10.3 挙動①／要件10.4 挙動③）。
    ///
    /// - mtime が変わったら read→`load_settings`:
    ///   - `Loaded` → current を更新し `emit('settings-changed')`（DTO）。無効値があれば追加で警告も emit。
    ///   - `KeepPrevious`（実行中の不完全保存）→ current を**据え置き** `emit('settings-warning')`
    ///     （既定への全戻しでちらつかせない＝要件10.4）。
    /// - read 失敗（削除/一時的に開けない）→ 据え置き（消えても直前/既定を維持＝データを失わない）。
    ///
    /// 起動を遅延ブロックしないよう専用スレッドで回す（最初のポーリングは POLL_INTERVAL 後）。
    pub fn spawn_watch(self: &Arc<Self>, app: AppHandle) {
        let me = Arc::clone(self);
        thread::spawn(move || {
            // 起動ロード済みファイルの mtime を初期値にし、その後の変化のみを反映する。
            let mut last_mtime = file_mtime_ms(&me.path);
            loop {
                thread::sleep(POLL_INTERVAL);
                let now_mtime = file_mtime_ms(&me.path);
                // mtime 不変（or 取得不能で同値）はスキップ。削除→不在も None 同士で短絡する。
                if now_mtime == last_mtime {
                    continue;
                }
                last_mtime = now_mtime;
                // mtime は動いたが read できない（削除/一時ロック）→ 据え置き（直前維持）。
                let Ok(text) = std::fs::read_to_string(&me.path) else {
                    continue;
                };
                match load_settings(&text) {
                    SettingsLoad::Loaded { settings, warnings } => {
                        if let Ok(mut g) = me.current.lock() {
                            *g = settings.clone();
                        }
                        // 設定変更を frontend へ通知（DTO）。受け側は段階的反映（本バッチは通知の貫通）。
                        let _ = app.emit("settings-changed", SettingsDto::from(settings));
                        if !warnings.is_empty() {
                            emit_invalid_keys_warning(&app, &warnings);
                        }
                    }
                    SettingsLoad::KeepPrevious { .. } => {
                        // 実行中の不完全保存 → current は据え置き（既定へ全戻ししない＝ちらつき防止）。
                        let _ = app.emit(
                            "settings-warning",
                            "保存途中の不完全な設定のため直前の設定を維持しています".to_string(),
                        );
                    }
                }
            }
        });
    }
}

/// 無効値キーを列挙した日本語警告を emit する（起動時・実行中で共通）。
fn emit_invalid_keys_warning(app: &AppHandle, warnings: &[String]) {
    let _ = app.emit(
        "settings-warning",
        format!(
            "設定の一部の値が無効なため既定値を使用します（{}）",
            warnings.join(", ")
        ),
    );
}

/// 現在の有効設定を frontend へ返す（要件10.3/10.4 設定取得 API）。
#[tauri::command]
pub fn get_settings(settings: State<'_, Arc<SettingsService>>) -> SettingsDto {
    settings.snapshot().into()
}

/// 機密判定の設定 `sensitive_patterns`（和集合・既定は外せない＝U2b-2）を managed state から取る。
///
/// preview/asset の custom protocol ハンドラが**逐語重複**していた取得（`try_state` →
/// `snapshot().sensitive_patterns` → `unwrap_or_default`）を 1 箇所へ集約する単一源。
/// `SettingsService` が未登録（後発プロセスの early-exit 等の極端な異常系）なら空 patterns を返す＝
/// **既定機密のみで安全側**（`is_sensitive_with` が常に既定を内包するため、空でも `.env` 等は拒否される
/// ＝機密判定を弱めない不変条件）。設定は既定へ「足すだけ」で外せない（U2b-2）。
pub(crate) fn sensitive_patterns_of(app: &AppHandle) -> Vec<String> {
    app.try_state::<Arc<SettingsService>>()
        .map(|s| s.snapshot().sensitive_patterns)
        .unwrap_or_default()
}

/// settings.toml の絶対パスを組み立てる（`<data_root>/settings.toml`・既存 state.json と同じ data_root）。
pub fn settings_path(root: &std::path::Path) -> PathBuf {
    root.join("settings.toml")
}

/// ファイルの mtime（ミリ秒）。不在/取得不能は None（ポーリングの変化検知に使う）。
fn file_mtime_ms(path: &std::path::Path) -> Option<u64> {
    let meta = std::fs::metadata(path).ok()?;
    meta.modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as u64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dto_への変換で列挙が文字列化される() {
        let s = Settings::default();
        let dto = SettingsDto::from(s);
        assert_eq!(dto.theme, "system");
        assert_eq!(dto.default_mode, "source");
        assert_eq!(dto.tab_width, 4);
        assert!(dto.wrap_default);
        assert_eq!(dto.excluded_dirs, vec![".git".to_string(), "node_modules".to_string()]);
    }

    #[test]
    fn settings_path_は_data_root_直下() {
        let p = settings_path(std::path::Path::new(r"C:\Users\u\AppData\Local\pika"));
        assert!(p.ends_with("settings.toml"));
        assert_eq!(
            p,
            PathBuf::from(r"C:\Users\u\AppData\Local\pika\settings.toml")
        );
    }
}
