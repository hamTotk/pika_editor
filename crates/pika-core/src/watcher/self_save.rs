//! 自己保存抑制（要件7.1/4.2・design doc 163行）。
//!
//! pika が保存する直前に「保存トークン」（パス＋保存後内容ハッシュ＋時刻）を登録する。
//! watcher 合成層は**内容ハッシュ一致を必須条件**として消し込む——
//! 現ディスク内容のハッシュが保存後ハッシュと一致する場合に自己イベントとして抑制する。
//!
//! - **1 回のアトミック保存は複数の自己イベントを撒く**（`MoveFileExW` 置換は対象パスに対し
//!   Removed/Modified/Created/rename-to のいずれか（複数）を撒く）。これらを**取りこぼさず全部飲む**
//!   ため、抑制はワンショット消費しない（トークンを残し、時刻窓の間は同一内容の自己イベントを
//!   何度でも抑制する）。これで「保存でファイル名に取り消し線（未読 removed）が付く」欠陥を塞ぐ。
//! - **時刻窓は補助安全弁**。`gc_expired` が窓超過トークンを掃除する（合成層が drain ごとに呼ぶ）。
//!   `decide` 自体は窓を見ない（窓超過でもハッシュ一致なら抑制＝リトライ・遅延書き込みを吸収）。
//! - ディスク内容が保存後ハッシュと**異なれば外部変更**として処理する（窓内でも）。
//! - 保存後ハッシュが取れないケース（巨大ファイル等）は外部変更として扱う（呼び出し側で
//!   `disk_hash=None` を渡せば本層は必ず外部変更と判定する）。
//!
//! 本層は I/O を行わない。内容ハッシュの算出は呼び出し側（document/保存層）が行い、
//! ここには「保存後ハッシュ」と「現ディスクハッシュ」を文字列で渡す（決定論を保つ）。

use std::collections::HashMap;

/// 既定の時刻窓（ミリ秒・補助安全弁）。GC のための上限であり抑制の主条件ではない。
pub const DEFAULT_TOKEN_WINDOW_MS: u64 = 5_000;

/// 保存直前に登録するトークン。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SaveToken {
    /// 保存後の内容ハッシュ（LF 正規化後・呼び出し側が算出）。
    pub saved_hash: String,
    /// 登録時刻（ミリ秒・単調増加）。
    pub registered_at_ms: u64,
}

/// イベントを自己保存として抑制するか/外部変更として処理するかの判定。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SuppressDecision {
    /// 自己保存イベント。抑制する（未読を付けない）。トークンは残し、窓内の後続自己イベントも飲む。
    Suppress,
    /// 外部変更。通常どおり処理する（未読化する）。
    External,
}

/// パスごとの保存トークン台帳。保存層が登録し、watcher 合成層が消し込む。
#[derive(Debug, Default)]
pub struct SaveTokenStore {
    window_ms: u64,
    tokens: HashMap<String, SaveToken>,
}

impl SaveTokenStore {
    /// 既定の時刻窓で台帳を作る。
    pub fn new() -> Self {
        Self::with_window(DEFAULT_TOKEN_WINDOW_MS)
    }

    /// 時刻窓を指定して台帳を作る（GC 用の補助安全弁の上限）。
    pub fn with_window(window_ms: u64) -> Self {
        Self {
            window_ms,
            tokens: HashMap::new(),
        }
    }

    /// 保存直前にトークンを登録する（同一パスは最新で上書き）。
    pub fn register(
        &mut self,
        path: impl Into<String>,
        saved_hash: impl Into<String>,
        now_ms: u64,
    ) {
        self.tokens.insert(
            path.into(),
            SaveToken {
                saved_hash: saved_hash.into(),
                registered_at_ms: now_ms,
            },
        );
    }

    /// 登録済みトークン数（テスト/診断用）。
    pub fn len(&self) -> usize {
        self.tokens.len()
    }

    /// 台帳が空か。
    pub fn is_empty(&self) -> bool {
        self.tokens.is_empty()
    }

    /// 指定パスに保存トークンがあるか（合成層が「ハッシュ採取（I/O）の前に」短絡判定するのに使う）。
    ///
    /// トークンが無い path（＝大多数の外部変更）には `hash_file` の I/O を回さず即 External 扱いにできる
    /// （ロック保持中の不要なファイル読みを避ける＝固まらない原則）。
    pub fn contains(&self, path: &str) -> bool {
        self.tokens.contains_key(path)
    }

    /// watcher イベントを自己保存として抑制すべきか判定する。
    ///
    /// - `disk_hash`: 現ディスク内容のハッシュ（取得不能なら `None`＝必ず外部変更）。
    /// - `now_ms`: イベント処理時刻。
    ///
    /// 判定（design doc 163行・要件7.1）:
    /// 1. トークンが無ければ外部変更。
    /// 2. **ハッシュ一致が必須**。一致しなければ外部変更（窓内でも内容相違は外部変更）。
    /// 3. ハッシュ一致なら自己保存として抑制する。**トークンは消費せず残す**——1 回のアトミック保存が
    ///    撒く複数の自己イベント（Removed+Created+Modified 等）を**すべて飲む**ため（ワンショットだと
    ///    1 件目で消費し、残る Removed が取り消し線を立ててしまう）。窓超過トークンは [`Self::gc_expired`]
    ///    が掃除する（`decide` は窓を見ない＝窓超過でも一致なら抑制＝リトライ・遅延書き込みを吸収）。
    ///    取りこぼし防止のため、`&mut self` 署名は維持する（将来の最終消費フックの余地・呼び出し側不変）。
    pub fn decide(
        &mut self,
        path: &str,
        disk_hash: Option<&str>,
        _now_ms: u64,
    ) -> SuppressDecision {
        let Some(token) = self.tokens.get(path) else {
            return SuppressDecision::External;
        };
        // 保存後ハッシュが取れない/一致しなければ外部変更（ハッシュ一致が必須条件）。
        let Some(disk) = disk_hash else {
            return SuppressDecision::External;
        };
        if disk != token.saved_hash {
            return SuppressDecision::External;
        }
        // 一致＝自己保存。トークンは残す（窓内の後続自己イベント＝Removed/Created/Modified を全部飲む）。
        SuppressDecision::Suppress
    }

    /// 時刻窓を超過した古いトークンを GC する（補助安全弁。抑制判定とは独立）。
    ///
    /// 自己イベントが何らかの理由で届かなかったトークンが台帳に残り続けるのを防ぐ。
    /// 返り値は GC した件数。
    pub fn gc_expired(&mut self, now_ms: u64) -> usize {
        let window = self.window_ms;
        let before = self.tokens.len();
        self.tokens
            .retain(|_, t| now_ms.saturating_sub(t.registered_at_ms) <= window);
        before - self.tokens.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ハッシュ一致は自己保存として抑制する() {
        let mut s = SaveTokenStore::with_window(5000);
        s.register("a.md", "hash-A", 1000);
        assert_eq!(
            s.decide("a.md", Some("hash-A"), 1050),
            SuppressDecision::Suppress
        );
    }

    #[test]
    fn 保存1回が撒く複数の自己イベントをすべて抑制する() {
        // 修正1: アトミック置換（MoveFileExW）は対象パスへ Removed/Created/Modified を連発する。
        // ワンショット消費だと 1 件目で消費し残る Removed が取り消し線（未読 removed）を立てていた。
        // トークンを残すことで、保存後ハッシュ一致の自己イベントを **何度でも** 抑制する（未読を付けない）。
        let mut s = SaveTokenStore::new();
        s.register("a.md", "hash-A", 0);
        // Removed → Created → Modified の 3 連発（atomic 置換が撒く自己イベント）を全部飲む。
        assert_eq!(
            s.decide("a.md", Some("hash-A"), 10),
            SuppressDecision::Suppress
        );
        assert_eq!(
            s.decide("a.md", Some("hash-A"), 20),
            SuppressDecision::Suppress
        );
        assert_eq!(
            s.decide("a.md", Some("hash-A"), 30),
            SuppressDecision::Suppress
        );
        // トークンは残っている（消費しない）。窓超過は gc_expired が掃除する。
        assert!(s.contains("a.md"));
        assert_eq!(s.len(), 1);
    }

    #[test]
    fn 抑制後の真の外部変更は内容相違で通す() {
        // トークンを残しても「内容が違う」自己イベント後の外部変更は External で通す（未読化する）。
        let mut s = SaveTokenStore::new();
        s.register("a.md", "hash-A", 0);
        // 自己保存（一致）は抑制。
        assert_eq!(
            s.decide("a.md", Some("hash-A"), 10),
            SuppressDecision::Suppress
        );
        // その後ディスクが別内容に変わった（真の外部変更）＝内容相違で通す。
        assert_eq!(
            s.decide("a.md", Some("hash-EXTERNAL"), 20),
            SuppressDecision::External
        );
    }

    #[test]
    fn 窓超過でもハッシュ一致なら抑制する() {
        // 時刻窓は補助安全弁。窓を超過しても一致していれば自己保存として抑制する。
        let mut s = SaveTokenStore::with_window(1000);
        s.register("a.md", "hash-A", 0);
        assert_eq!(
            s.decide("a.md", Some("hash-A"), 999_999),
            SuppressDecision::Suppress
        );
    }

    #[test]
    fn 窓内でも内容相違なら外部変更() {
        // 窓内でもハッシュが違えば外部変更（ハッシュ一致が必須条件）。
        let mut s = SaveTokenStore::with_window(5000);
        s.register("a.md", "hash-A", 1000);
        assert_eq!(
            s.decide("a.md", Some("hash-EXTERNAL"), 1100),
            SuppressDecision::External
        );
        // 外部変更判定はトークンを消費しない（自己保存はまだ来ていないかもしれない）。
        assert_eq!(s.len(), 1);
    }

    #[test]
    fn 保存後ハッシュが取れないケースは外部変更() {
        let mut s = SaveTokenStore::new();
        s.register("big.md", "hash-A", 0);
        assert_eq!(s.decide("big.md", None, 10), SuppressDecision::External);
    }

    #[test]
    fn トークンなしは外部変更() {
        let mut s = SaveTokenStore::new();
        assert_eq!(
            s.decide("unknown.md", Some("x"), 0),
            SuppressDecision::External
        );
    }

    #[test]
    fn 期限切れトークンは_gc_される() {
        let mut s = SaveTokenStore::with_window(1000);
        s.register("a.md", "hash-A", 0);
        // 窓内なら GC されない。
        assert_eq!(s.gc_expired(500), 0);
        assert_eq!(s.len(), 1);
        // 窓超過で GC される。
        assert_eq!(s.gc_expired(2000), 1);
        assert!(s.is_empty());
    }
}
