//! 巨大ファイルのストリーミング range 読取（第2段階 仮想化ビューア・要件2.2・design doc 8章）。
//!
//! 第2段階（[`crate::huge::FileStage::Stage2ReadOnly`]）は CM6 に全量ロードせず、Rust の
//! バイト範囲読取＋フロントの仮想化ウィンドウビューアで**読み取り専用**閲覧する（ログ/JSON
//! ビューア型）。本モジュールは「どのバイト範囲を読むか」「行境界でどう整えるか」を決める
//! **純粋ロジック**で、実 I/O（custom protocol range / Channel）は src-tauri 側が担う
//! （pika-core は FS を握らない＝design doc 4章「コアは UI を知らない」）。
//!
//! 仮想化ビューアの設計（design doc 8章）:
//! - フロントは「N 行目あたりを見たい」という要求を出す。コアは要求行近傍の**バイト範囲**を
//!   返し、src-tauri がその範囲だけ読んで返す（全量を読まない＝固まらない・メモリ爆発しない）。
//! - 範囲は UTF-8 のマルチバイト境界を割らないよう、読んだバイト列の前後で**行頭/行末に整える**。
//! - 編集/保存/置換は不可（読み取り専用＝要件2.2 第2段階・5.4）。

/// 仮想化ウィンドウ 1 枚分の読み取り範囲（バイト・半開区間 `[start, end)`）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ByteRange {
    /// 開始バイト（含む）。
    pub start: u64,
    /// 終了バイト（含まない）。
    pub end: u64,
}

impl ByteRange {
    /// 範囲の長さ（バイト）。
    pub fn len(self) -> u64 {
        self.end.saturating_sub(self.start)
    }

    /// 空範囲か。
    pub fn is_empty(self) -> bool {
        self.end <= self.start
    }
}

/// 仮想化ビューアの 1 ウィンドウの既定バイト幅（design doc 8章「仮想化ウィンドウ」）。
///
/// 1 回の range 読取で運ぶ最大バイト数。大きすぎると IPC/描画が重く、小さすぎるとスクロールで
/// 読取回数が増える。1MB を既定とする（CM6 を介さない素朴ビューアなので 1MB でも軽い）。
pub const DEFAULT_WINDOW_BYTES: u64 = 1024 * 1024;

/// 行頭/行末整列のために前後へ広げる最大バイト数（長行で無限に広がらないための上限）。
///
/// 改行が見つからない巨大 1 行（AI 出力の単一行 JSON）でも、ここで頭打ちにして範囲を確定する
/// （途中で切れても仮想化ビューアは「読み取り専用の素朴表示」なので破綻しない）。
pub const ALIGN_SCAN_LIMIT_BYTES: u64 = 64 * 1024;

/// 表示したいオフセットを中心に、ウィンドウ幅のバイト範囲を計算する（ファイルサイズでクランプ）。
///
/// - `center`: 見たい位置のバイトオフセット（フロントのスクロール位置から算出）。
/// - `window`: ウィンドウ幅（バイト・0 のときは [`DEFAULT_WINDOW_BYTES`]）。
/// - `file_size`: ファイル全体のバイトサイズ。
///
/// `center` を中心に前後半分ずつ取り、ファイル端でクランプする（負やサイズ超で破綻しない）。
/// 行境界整列は [`align_to_lines`] で別途行う（範囲算出と整列を分離してテストしやすくする）。
pub fn window_around(center: u64, window: u64, file_size: u64) -> ByteRange {
    let window = if window == 0 {
        DEFAULT_WINDOW_BYTES
    } else {
        window
    };
    if file_size == 0 {
        return ByteRange { start: 0, end: 0 };
    }
    let half = window / 2;
    let center = center.min(file_size);
    let start = center.saturating_sub(half);
    let end = start.saturating_add(window).min(file_size);
    // end をクランプした分だけ start を前へ戻し、可能ならウィンドウ幅を確保する。
    let start = end.saturating_sub(window).min(start);
    ByteRange { start, end }
}

/// 読み取ったバイト列（`raw`・`base` から始まる範囲）を**行頭/行末**に整える（要件2.2 第2段階）。
///
/// 仮想化ビューアは行単位で表示するため、ウィンドウ境界が行の途中だと半端な行が出る。読み取った
/// バイト列の中で、先頭側は最初の改行の**直後**まで捨て、末尾側は最後の改行の**直後**で切る。
/// これにより返す範囲は「完全な行の連なり」になる。改行が全く無い（巨大 1 行）の場合は
/// [`ALIGN_SCAN_LIMIT_BYTES`] までで諦めて全体を返す（読み取り専用の素朴表示なので可）。
///
/// - `base`: `raw` の先頭が対応するファイル内バイトオフセット。
/// - `raw`: `window_around` で読んだバイト列。
/// - 戻り: 整列後の `ByteRange`（`base..base+raw.len()` の内側）。
pub fn align_to_lines(base: u64, raw: &[u8]) -> ByteRange {
    if raw.is_empty() {
        return ByteRange {
            start: base,
            end: base,
        };
    }
    // 先頭側: base が 0 でなければ最初の `\n` の直後まで捨てる（前のウィンドウが行末まで持つ前提）。
    let mut lead = 0usize;
    if base > 0 {
        let scan = raw.len().min(ALIGN_SCAN_LIMIT_BYTES as usize);
        if let Some(pos) = raw[..scan].iter().position(|&b| b == b'\n') {
            lead = pos + 1; // 改行の直後から表示する。
        }
        // 改行が見つからなければ lead=0（巨大1行・全体を返す）。
    }
    // 末尾側: 最後の `\n` の直後で切る（半端な行を末尾に残さない）。
    let scan_from = raw.len().saturating_sub(ALIGN_SCAN_LIMIT_BYTES as usize);
    let trail_end = raw[scan_from..]
        .iter()
        .rposition(|&b| b == b'\n')
        .map(|p| scan_from + p + 1);
    let end_off = match trail_end {
        Some(e) if e > lead => e,
        // 末尾に改行が無い（巨大1行 or 最終行が改行なし）→ そのまま末尾まで。
        _ => raw.len(),
    };
    ByteRange {
        start: base + lead as u64,
        end: base + end_off as u64,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ウィンドウは中心の前後を取る() {
        let r = window_around(5000, 1000, 1_000_000);
        assert_eq!(r.start, 4500);
        assert_eq!(r.end, 5500);
        assert_eq!(r.len(), 1000);
    }

    #[test]
    fn ウィンドウはファイル先頭でクランプする() {
        let r = window_around(100, 1000, 1_000_000);
        assert_eq!(r.start, 0);
        assert_eq!(r.end, 1000); // 先頭でクランプしてもウィンドウ幅を確保。
    }

    #[test]
    fn ウィンドウはファイル末尾でクランプする() {
        let r = window_around(999_900, 1000, 1_000_000);
        assert_eq!(r.end, 1_000_000);
        assert_eq!(r.start, 999_000); // 末尾でクランプし幅を後ろへ確保。
    }

    #[test]
    fn ウィンドウは中心がサイズ超でも破綻しない() {
        let r = window_around(u64::MAX, 1000, 10_000);
        assert!(r.end <= 10_000);
        assert!(r.start <= r.end);
    }

    #[test]
    fn 空ファイルは空範囲() {
        let r = window_around(0, 1000, 0);
        assert!(r.is_empty());
    }

    #[test]
    fn ウィンドウ幅_0_は既定幅() {
        let r = window_around(2_000_000, 0, 10_000_000);
        assert_eq!(r.len(), DEFAULT_WINDOW_BYTES);
    }

    #[test]
    fn 行整列は先頭と末尾を行境界に揃える() {
        // base=10（先頭でない）→ 最初の改行直後から、最後の改行直後まで。
        let raw = b"xx\nline1\nline2\nyy"; // 先頭 "xx" は前行の途中、末尾 "yy" は次行の途中。
        let r = align_to_lines(10, raw);
        // 先頭: "xx\n" の 3 バイトを捨てて base+3 から。
        assert_eq!(r.start, 13);
        // 末尾: 最後の "\n"（"line2\n" の後）直後で切る＝"yy" を捨てる。
        // raw 内インデックス: "xx\n"=3, "line1\n"=6, "line2\n"=6 → 末尾改行 off=15。
        assert_eq!(r.end, 10 + 15);
    }

    #[test]
    fn 行整列は先頭ファイルでは先頭を捨てない() {
        // base=0（ファイル先頭）→ 先頭はそのまま、末尾だけ行境界へ。
        let raw = b"line1\nline2\npartial";
        let r = align_to_lines(0, raw);
        assert_eq!(r.start, 0);
        assert_eq!(r.end, 12); // "line1\nline2\n" まで（partial を捨てる）。
    }

    #[test]
    fn 行整列は改行なしの巨大1行を全体返しする() {
        // 改行が全く無い（巨大 1 行）→ lead=0・末尾まで（途中で切らず素朴表示）。
        let raw = vec![b'a'; 1000];
        let r = align_to_lines(0, &raw);
        assert_eq!(r.start, 0);
        assert_eq!(r.end, 1000);
    }

    #[test]
    fn 行整列は空入力で空範囲() {
        let r = align_to_lines(42, b"");
        assert_eq!(r, ByteRange { start: 42, end: 42 });
    }
}
