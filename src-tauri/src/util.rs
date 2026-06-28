//! src-tauri 層の小さな共通ユーティリティ（Win32 境界の UTF-16 変換・単調化ミリ秒時計）。
//!
//! 役割（design doc 3章「薄い橋渡し層」）: 各モジュールへ同型コピーが散在していた2つの定型を
//! 1か所へ集約する。ロジックではなく境界変換/時刻取得の最小ヘルパなので pika-core ではなく
//! アプリ層（src-tauri）に置く。
//! - [`to_wide`]: `encode_wide().chain(once(0)).collect()`（Win32 API の NUL 終端 UTF-16）。
//!   旧実装は document/jumplist/single_instance/webview2/snapshot_persist/watcher に同一コピーが散在。
//! - [`now_ms`]: 退避 created_at・監視の時間窓判定に使う**単調化**ミリ秒時計。旧 snapshot.rs の
//!   単調版（クロック後退で 0 に落とさない）へ統一し、素朴版だった watcher.rs もこれを使う。

use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

/// UTF-8/OsStr → NUL 終端 UTF-16（Win32 API 境界変換・CLAUDE.md「Win32 境界で UTF-16 に変換」）。
///
/// `&str`/`&Path`/`&OsStr` を共通に受け取れるよう `AsRef<OsStr>` を取る。終端 NUL を必ず付ける
/// （`CreateFileW`/`MoveFileExW`/`RegOpenKeyExW` 等は NUL 終端ワイド文字列を要求する）。
#[cfg(windows)]
pub fn to_wide(s: impl AsRef<std::ffi::OsStr>) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    s.as_ref().encode_wide().chain(std::iter::once(0)).collect()
}

/// これまでに [`now_ms`] が返した最大値（単調性のフロア）。
///
/// 退避の created_at_ms は容量GC の14日保護窓判定に使う。クロックが UNIX_EPOCH 前へ後退すると
/// `duration_since` が Err → 旧素朴版は 0 を返し、created_at_ms=0 の退避が量産されて保護窓が
/// 外れていた（#16）。本フロアで単調化し、後退・0 化を防ぐ。
static NOW_MS_FLOOR: AtomicU64 = AtomicU64::new(0);

/// 現在時刻（ミリ秒・単調化）。退避の created_at や監視の時間窓判定に使う。
///
/// 取得失敗（クロック後退）時も 0 へ落とさず、直近に返した値を再利用して単調性を壊さない（#16）。
/// 壁時計が取れたら prev とのより大きい方、取れなければ prev を採用する。
pub fn now_ms() -> u64 {
    let wall = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .ok();
    let prev = NOW_MS_FLOOR.load(Ordering::Relaxed);
    // 壁時計が取れたら prev とのより大きい方、取れなければ prev を採用（後退・0 化を防ぐ）。
    let value = wall.map(|w| w.max(prev)).unwrap_or(prev);
    // フロアを前進させる（並行更新でも最大値へ収束させる）。
    NOW_MS_FLOOR.fetch_max(value, Ordering::Relaxed);
    value
}
