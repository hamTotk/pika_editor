//! タスクバーのジャンプリスト（最近使った項目）配線（要件10.2・design doc 9章/19章）。
//!
//! ジャンプリストの **並び順・重複排除・上限** の決定論ロジックは pika_core::recent
//! （cargo test 済み）に置き、ここには OS への登録（`SHAddToRecentDocs`）の薄い FFI のみを置く
//! （design doc 3章「薄い橋渡し層」）。実機表示は系統C で確認する（GUI/タスクバー実描画）。
//!
//! 設計判断（YAGNI）: 完全な `ICustomDestinationList`（カスタムカテゴリ・タスク）COM ではなく、
//! まず `SHAddToRecentDocs`（OS の Recent/「最近使ったもの」ジャンプリストへ追加する 1 API）で
//! 軽量に実現する。pika は「最近使ったファイル/フォルダ」を出せれば足り、カスタムカテゴリは
//! 要件にない（要件14章「足さない」）。COM の重い配線を入れない分、保守とバイナリを軽く保つ。

/// 最近使ったパスを OS のジャンプリスト（Recent）へ登録する。
///
/// 非 Windows ではノーオペ。失敗は握り潰す（ジャンプリストはベストエフォート＝最上位原則に無関係）。
pub fn add_recent(path: &str) {
    #[cfg(windows)]
    windows_impl::add_recent(path);
    #[cfg(not(windows))]
    let _ = path;
}

#[cfg(windows)]
mod windows_impl {
    use windows_sys::Win32::UI::Shell::{SHAddToRecentDocs, SHARD_PATHW};

    /// `SHAddToRecentDocs(SHARD_PATHW, <wide path>)` で OS の Recent ジャンプリストへ追加する。
    pub fn add_recent(path: &str) {
        if path.trim().is_empty() {
            return;
        }
        let wide = crate::util::to_wide(path);
        // SAFETY: wide は NUL 終端のパス文字列。SHARD_PATHW は wide ポインタを取る。
        // 失敗時も副作用なし（戻り値は使わない＝ベストエフォート）。
        // SHARD_PATHW は i32 定数だが API は uFlags: u32 を取るのでキャストする。
        unsafe {
            SHAddToRecentDocs(SHARD_PATHW as u32, wide.as_ptr() as *const _);
        }
    }
}
