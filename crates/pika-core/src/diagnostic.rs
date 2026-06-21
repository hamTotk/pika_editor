//! 診断ログの方針（要件12.3・design doc 19章 診断ログ）。
//!
//! 本モジュールは UI/Tauri/wry/FS を一切知らない**純粋ロジック**（cargo test の決定論ゲート対象）。
//! 「どのレベルを記録するか／ログフォルダのパス／ローテーション要否／ユーザー内容を書かない整形」の
//! **意思決定**だけを担い、実際のファイル追記・ローテーション実行は呼び出し側（`src-tauri`）が
//! [`RotationPlan`]・[`LogLine`] に従って行う。
//!
//! 要件12.3:
//! - error/warn/info をデータルート配下 `logs/` に記録（既定は warn 以上）。
//! - **ログにユーザーのファイル内容を書き込まない**（ファイルパス・操作・エラー情報のみ）。
//! - **1ファイル5MB・3世代のローテーション**目安。
//! - メニューからログフォルダを開ける（パス解決のみ本モジュール）。

use std::path::{Path, PathBuf};

/// ログレベル（要件12.3）。数値が大きいほど深刻。
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum LogLevel {
    /// 情報（操作トレース・既定では記録しない）。
    Info,
    /// 警告（既定の記録下限）。
    Warn,
    /// エラー（必ず記録）。
    Error,
}

impl LogLevel {
    /// ログ行の接頭辞（"INFO"/"WARN"/"ERROR"）。
    pub fn label(self) -> &'static str {
        match self {
            LogLevel::Info => "INFO",
            LogLevel::Warn => "WARN",
            LogLevel::Error => "ERROR",
        }
    }
}

/// 1ファイルの上限（バイト）。これを**超える**とローテーションする（要件12.3「1ファイル5MB」目安）。
pub const MAX_LOG_BYTES: u64 = 5 * 1024 * 1024;

/// 保持世代数（要件12.3「3世代」）。現行 + 過去 (世代-1) 本を保持する。
pub const MAX_GENERATIONS: usize = 3;

/// 現行ログファイル名。
pub const CURRENT_LOG_NAME: &str = "pika.log";

/// データルートからログフォルダ（`<data_root>/logs/`）を解決する（要件12.3・design doc 9章）。
///
/// 「メニューからログフォルダを開ける」導線のパスを純粋関数で決める（実 mkdir/open は呼び出し側）。
pub fn log_dir(data_root: &Path) -> PathBuf {
    data_root.join("logs")
}

/// 現行ログファイルのパス（`<data_root>/logs/pika.log`）。
pub fn current_log_path(data_root: &Path) -> PathBuf {
    log_dir(data_root).join(CURRENT_LOG_NAME)
}

/// 既定の記録下限（要件12.3「既定 warn 以上」）。
pub const DEFAULT_MIN_LEVEL: LogLevel = LogLevel::Warn;

/// この `level` を `min_level` 設定のもとで記録するか（要件12.3 既定 warn 以上）。
pub fn should_log(level: LogLevel, min_level: LogLevel) -> bool {
    level >= min_level
}

/// ログ1行のための整形済みレコード（**ユーザーのファイル内容を持たない**＝要件12.3）。
///
/// 構造的に「内容フィールドを持たない」ことで、文書本文をログへ書き込む経路を型レベルで塞ぐ。
/// 入るのはレベル・カテゴリ・操作名・対象パス（パス自体は内容でない）・エラー要約のみ。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogLine {
    /// レベル。
    pub level: LogLevel,
    /// カテゴリ（"watcher"/"snapshot"/"encoding" 等の発生源・短い識別子）。
    pub category: String,
    /// 操作名（"save"/"open"/"confirm" 等・短い識別子）。
    pub op: String,
    /// 対象ファイルパス（任意・パスは「内容」でないので記録可。要件12.3）。
    pub path: Option<String>,
    /// エラー/警告の要約（**ファイル本文を入れない**。呼び出し側がメッセージのみ渡す）。
    pub summary: String,
}

impl LogLine {
    /// 1行のログ文字列へ整形する（タイムスタンプは呼び出し側が前置するため含めない）。
    ///
    /// 形式: `LEVEL [category] op path=<path> - <summary>`。改行は呼び出し側が付ける。
    /// summary 内の改行は空白へ畳んで1行を保つ（ログ行の崩れ・本文流入の二次防止）。
    pub fn format(&self) -> String {
        let path = self
            .path
            .as_deref()
            .map(|p| format!(" path={p}"))
            .unwrap_or_default();
        let one_line_summary = self.summary.replace(['\n', '\r'], " ");
        format!(
            "{} [{}] {}{} - {}",
            self.level.label(),
            self.category,
            self.op,
            path,
            one_line_summary
        )
    }
}

/// ローテーションの指示（呼び出し側が従う）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RotationPlan {
    /// 追記のみ（上限未満）。
    AppendOnly,
    /// ローテーションしてから追記する。
    Rotate {
        /// 削除する最古世代のファイル名（保持上限超過分・None なら削除不要）。
        delete: Option<String>,
        /// `(from, to)` のリネーム指示列（`pika.log.1 → pika.log.2`・`pika.log → pika.log.1` 等）。
        /// 最古→最新の順に適用すれば衝突しない並びで返す。
        renames: Vec<(String, String)>,
    },
}

/// 世代付きログファイル名（`pika.log` / `pika.log.1` / `pika.log.2` ...）。
fn generation_name(gen: usize) -> String {
    if gen == 0 {
        CURRENT_LOG_NAME.to_string()
    } else {
        format!("{CURRENT_LOG_NAME}.{gen}")
    }
}

/// 追記前にローテーションが要るかを判定し、必要なら世代繰り上げ計画を返す（要件12.3）。
///
/// - `current_size`: 現行 `pika.log` のバイトサイズ。
/// - `incoming_len`: これから追記する1行のバイト長。
///
/// 現行サイズ + 追記分が [`MAX_LOG_BYTES`] を**超える**ならローテーションする
/// （`pika.log.(N-1)` を捨て、`pika.log.k → pika.log.(k+1)`、`pika.log → pika.log.1`）。
/// 保持は現行 + 過去 ([`MAX_GENERATIONS`] - 1) 本（合計 3 世代）。
pub fn plan_rotation(current_size: u64, incoming_len: u64) -> RotationPlan {
    if current_size + incoming_len <= MAX_LOG_BYTES {
        return RotationPlan::AppendOnly;
    }
    // 保持上限を超える最古世代（pika.log.(MAX_GENERATIONS-1)）は削除。
    let oldest = MAX_GENERATIONS - 1;
    let delete = Some(generation_name(oldest));
    // 最古から繰り上げて衝突しない並び: pika.log.(oldest-1) → pika.log.oldest, ... , pika.log → pika.log.1。
    let mut renames = Vec::new();
    for gen in (0..oldest).rev() {
        renames.push((generation_name(gen), generation_name(gen + 1)));
    }
    RotationPlan::Rotate { delete, renames }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 既定は_warn_以上を記録し_info_は記録しない() {
        // 要件12.3「既定 warn 以上」。
        assert!(!should_log(LogLevel::Info, DEFAULT_MIN_LEVEL));
        assert!(should_log(LogLevel::Warn, DEFAULT_MIN_LEVEL));
        assert!(should_log(LogLevel::Error, DEFAULT_MIN_LEVEL));
    }

    #[test]
    fn 設定で_info_まで下げれば_info_も記録する() {
        assert!(should_log(LogLevel::Info, LogLevel::Info));
        assert!(should_log(LogLevel::Warn, LogLevel::Info));
    }

    #[test]
    fn ログフォルダはデータルート配下の_logs() {
        let dir = log_dir(Path::new(r"C:\Users\u\AppData\Local\pika"));
        assert!(dir.ends_with("logs"));
        let cur = current_log_path(Path::new(r"C:\Users\u\AppData\Local\pika"));
        assert!(cur.ends_with(r"logs\pika.log") || cur.ends_with("logs/pika.log"));
    }

    #[test]
    fn ログ行はユーザー内容を持たずパスと要約のみ整形する() {
        // 要件12.3「ファイル内容を書き込まない」。型に content フィールドが無いことを整形でも担保。
        let line = LogLine {
            level: LogLevel::Error,
            category: "save".into(),
            op: "atomic_write".into(),
            path: Some(r"C:\work\notes.md".into()),
            summary: "アクセスが拒否されました".into(),
        };
        let s = line.format();
        assert!(s.starts_with("ERROR [save] atomic_write"));
        assert!(s.contains(r"path=C:\work\notes.md"));
        assert!(s.contains("アクセスが拒否されました"));
    }

    #[test]
    fn 要約の改行は1行へ畳んでログ行を崩さない() {
        let line = LogLine {
            level: LogLevel::Warn,
            category: "settings".into(),
            op: "parse".into(),
            path: None,
            summary: "1行目\n2行目\r\n3行目".into(),
        };
        let s = line.format();
        assert!(!s.contains('\n'));
        assert!(!s.contains('\r'));
    }

    #[test]
    fn 上限未満なら追記のみ() {
        assert_eq!(plan_rotation(0, 100), RotationPlan::AppendOnly);
        assert_eq!(
            plan_rotation(MAX_LOG_BYTES - 1, 1),
            RotationPlan::AppendOnly
        );
    }

    #[test]
    fn 上限超で3世代ローテーションし最古を削除する() {
        // 5MB 超で pika.log → pika.log.1、pika.log.1 → pika.log.2、pika.log.2 を削除。
        let plan = plan_rotation(MAX_LOG_BYTES, 1);
        match plan {
            RotationPlan::Rotate { delete, renames } => {
                assert_eq!(delete, Some("pika.log.2".to_string()));
                // 最古から繰り上げ（衝突しない順）。
                assert_eq!(
                    renames,
                    vec![
                        ("pika.log.1".to_string(), "pika.log.2".to_string()),
                        ("pika.log".to_string(), "pika.log.1".to_string()),
                    ]
                );
            }
            _ => panic!("ローテーションになるはず"),
        }
    }

    #[test]
    fn ローテーションは世代を超えて増やさない() {
        // 3世代固定。renames は (MAX_GENERATIONS-1) 本（pika.log.1→2, pika.log→1）。
        if let RotationPlan::Rotate { renames, .. } = plan_rotation(MAX_LOG_BYTES + 1, 0) {
            assert_eq!(renames.len(), MAX_GENERATIONS - 1);
        } else {
            panic!("ローテーションになるはず");
        }
    }
}
