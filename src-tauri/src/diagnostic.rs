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

/// 現在時刻を UTC ISO-8601（ミリ秒・末尾 Z）で返す（本文に含めず行頭にだけ置く）。
///
/// chrono 等を足さず軽量に保つ（依存追加はサイズと天秤＝CLAUDE.md）。epoch ms を純粋な暦演算で
/// 変換する（[`epoch_ms_to_iso`]）。クロック取得失敗時は epoch 0（1970-01-01T00:00:00.000Z）扱い。
fn now_iso_like() -> String {
    let ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    epoch_ms_to_iso(ms)
}

/// UNIX epoch ミリ秒を UTC の ISO-8601（`YYYY-MM-DDTHH:MM:SS.mmmZ`）へ純粋関数で変換する。
///
/// chrono を足さず軽量に保つため、Howard Hinnant の `civil_from_days` アルゴリズムで
/// epoch 日数→(年, 月, 日) を求め、日内の時分秒ミリは剰余で算出する（うるう年・月日繰り上げは
/// civil 変換が自動的に正しく扱う）。負の epoch（1970 以前）は考慮不要なので 0 扱いに倒す。
fn epoch_ms_to_iso(ms: u128) -> String {
    // 日内成分（時分秒ミリ）は 1 日 = 86_400_000ms の剰余、暦日は商で求める。
    let secs_total = ms / 1000;
    let ms3 = (ms % 1000) as u32;
    let days = (secs_total / 86_400) as i64;
    let secs_of_day = (secs_total % 86_400) as u32;
    let h = secs_of_day / 3600;
    let m = (secs_of_day % 3600) / 60;
    let s = secs_of_day % 60;

    // civil_from_days（Howard Hinnant）: epoch 日数（1970-01-01 = 0）→ (year, month, day)。
    // 3 月始まりの暦年（era）で計算し、最後に 1〜2 月を前年へ繰り上げ補正する。
    let z = days + 719_468; // 0000-03-01 を起点に平行移動（719_468 = 1970-01-01 までの日数）。
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097; // 400 年周期（146_097 日）。
    let doe = (z - era * 146_097) as u64; // era 内の日（0..=146_096）。
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365; // era 内の年（0..=399）。
    let year_civil = yoe as i64 + era * 400; // 3 月始まり暦の年。
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // 3 月始まり年内の日（0..=365）。
    let mp = (5 * doy + 2) / 153; // 3 月始まりの月インデックス（0=3 月 .. 11=2 月）。
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32; // 日（1..=31）。
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32; // 通常の月（1..=12）へ戻す。
    let year = if month <= 2 { year_civil + 1 } else { year_civil }; // 1〜2 月は翌暦年。

    format!("{year:04}-{month:02}-{day:02}T{h:02}:{m:02}:{s:02}.{ms3:03}Z")
}

#[cfg(test)]
mod tests {
    use super::epoch_ms_to_iso;

    #[test]
    fn epoch_0_は_1970年元日() {
        assert_eq!(epoch_ms_to_iso(0), "1970-01-01T00:00:00.000Z");
    }

    #[test]
    fn 既知値_1_7e12ms_は_2023_11_14() {
        // 1_700_000_000_000ms = 1_700_000_000s。
        // 1_700_000_000 / 86_400 = 19_675 日 余り 80_000 秒。
        // 80_000 秒 = 22h13m20s。epoch 19_675 日目 = 2023-11-14（広く知られた epoch 1.7e9 の値と一致）。
        assert_eq!(
            epoch_ms_to_iso(1_700_000_000_000),
            "2023-11-14T22:13:20.000Z"
        );
    }

    #[test]
    fn ミリ秒成分が3桁ゼロ詰めで残る() {
        // 端数ミリ（7ms）が .007 として保持される。
        assert_eq!(epoch_ms_to_iso(7), "1970-01-01T00:00:00.007Z");
        assert_eq!(epoch_ms_to_iso(1_500), "1970-01-01T00:00:01.500Z");
    }

    #[test]
    fn うるう年境界_2020_02_29_を正しく扱う() {
        // 2020-02-29T12:34:56.000Z の epoch ms を逆算して検算する。
        // 1970→2020 の経過日数（うるう年 1972,1976,...,2016 の +13 日込み）を積み上げる。
        let days_1970_to_2020: i64 = {
            let mut d = 0i64;
            for y in 1970..2020 {
                d += if (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 {
                    366
                } else {
                    365
                };
            }
            d
        };
        // 2020-02-29 = 2020-01-01 + 31(1月) + 28(2月 1〜28 日) 日 = +59 日。
        let days = days_1970_to_2020 + 31 + 28;
        let secs = days as u128 * 86_400 + 12 * 3600 + 34 * 60 + 56;
        assert_eq!(
            epoch_ms_to_iso(secs * 1000),
            "2020-02-29T12:34:56.000Z",
            "うるう年 2 月 29 日の月日が正しく繰り上がる"
        );
    }
}
