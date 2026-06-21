//! 診断ログの配線（要件12.3・design doc 19章 診断ログ）。
//!
//! 役割（design doc 3章「薄い境界」）:
//! - レベル判定・ログフォルダのパス解決・ローテーション計画・ユーザー内容を書かない整形は
//!   **すべて pika-core::diagnostic**（cargo test 済み）に委ね、ここは FS 追記/ローテーション実行と
//!   「ログフォルダを開く」OS 呼び出しに徹する。
//! - ログにユーザーのファイル内容を書かない（[`pika_core::diagnostic::LogLine`] が構造的に内容を持たない）。

use pika_core::diagnostic::{
    self, current_log_path, log_dir, plan_rotation, LogLevel, LogLine, RotationPlan,
    DEFAULT_MIN_LEVEL,
};
use std::io::Write;
use std::path::Path;
use std::sync::{Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

/// 診断ログの書込を直列化する専用ロック（eval medium: 並行書込で世代ずれ/行取りこぼし/5MB超過 append を防ぐ）。
///
/// watcher/command/search の複数スレッドが同時に `record()` を呼ぶと、ローテーション判定
/// （metadata→delete→rename）と append が交錯しうる。`log_with_min` 全体をこのロックで囲み、
/// 「サイズ確認→ローテーション→追記」を 1 つの不可分区間にする。診断は副次データなので、
/// 万一ロックが毒された（panic 跨ぎ）場合でも into_inner で続行しアプリを止めない（固まらない）。
fn log_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

/// データルートを解決してから1行ログする（command 層からの簡便呼び出し）。
///
/// データルート解決に失敗してもアプリは止めない（診断は副次・固まらない）。ユーザー内容は入らない
/// （[`LogLine`] が path/op/summary のみ・本文フィールドを持たない＝要件12.3）。
pub fn record(level: LogLevel, category: &str, op: &str, path: Option<&str>, summary: &str) {
    let Ok(root) = crate::state_store::resolve() else {
        return;
    };
    log(
        &root.path,
        LogLine {
            level,
            category: category.to_string(),
            op: op.to_string(),
            path: path.map(|p| p.to_string()),
            summary: summary.to_string(),
        },
    );
}

/// 1 行のログを記録する（要件12.3）。`min_level` 未満は記録しない（既定 warn 以上）。
///
/// 追記前に [`plan_rotation`] でローテーション要否を判定し、必要なら世代繰り上げ＋最古削除を実行する
/// （1ファイル5MB・3世代＝要件12.3）。ファイル本文は決して入らない（型で担保）。
pub fn log(data_root: &Path, line: LogLine) {
    log_with_min(data_root, line, DEFAULT_MIN_LEVEL);
}

/// 記録下限を明示してログする（設定で warn→info へ下げるケース用）。
pub fn log_with_min(data_root: &Path, line: LogLine, min_level: LogLevel) {
    if !diagnostic::should_log(line.level, min_level) {
        return;
    }
    // 「サイズ確認→ローテーション→追記」を 1 つの不可分区間にする（並行書込の交錯防止・eval medium）。
    // ロックが毒されていても診断のためにアプリを止めない（into_inner で続行）。
    let _guard = log_lock().lock().unwrap_or_else(|e| e.into_inner());
    // ログフォルダを用意（無ければ作る・ワークスペースは汚さずデータルート配下のみ＝要件13）。
    let dir = log_dir(data_root);
    if std::fs::create_dir_all(&dir).is_err() {
        return; // ログ失敗でアプリを止めない（診断は副次・固まらない）。
    }
    let target = current_log_path(data_root);

    // タイムスタンプを前置して1行に整形（pika-core が本文を持たないことを保証する）。
    let formatted = format!("{} {}\n", now_iso_like(), line.format());
    let incoming_len = formatted.len() as u64;

    // 追記前にローテーション判定（要件12.3）。
    let current_size = std::fs::metadata(&target).map(|m| m.len()).unwrap_or(0);
    if let RotationPlan::Rotate { delete, renames } = plan_rotation(current_size, incoming_len) {
        if let Some(name) = delete {
            let _ = std::fs::remove_file(dir.join(name));
        }
        for (from, to) in renames {
            let _ = std::fs::rename(dir.join(from), dir.join(to));
        }
    }

    if let Ok(mut f) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&target)
    {
        let _ = f.write_all(formatted.as_bytes());
    }
}

/// 「メニューからログフォルダを開ける」導線（要件12.3）。
///
/// ログフォルダ（`<data_root>/logs/`）を作成（無ければ）してから、その絶対パスを返す。
/// 実際にエクスプローラーで開くのはフロント/OS シェルだが、ここでパスを確定して返す。
#[tauri::command]
pub fn log_folder_path() -> Result<String, String> {
    let root = crate::state_store::resolve()?;
    let dir = log_dir(&root.path);
    std::fs::create_dir_all(&dir).map_err(|e| format!("ログフォルダ作成に失敗: {e}"))?;
    Ok(dir.to_string_lossy().to_string())
}

/// 簡易タイムスタンプ（UTC 経過ミリ秒・本文に含めず行頭にだけ置く）。
///
/// chrono 等を足さず軽量に保つ（依存追加はサイズと天秤＝CLAUDE.md）。整列・並び替え用途に十分。
fn now_iso_like() -> String {
    let ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    format!("t={ms}")
}
