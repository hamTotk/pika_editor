//! UI/Tauri 非依存の小さな純粋ユーティリティ（座標変換・バイト整形）。cargo test の決定論ゲート対象。
//!
//! app 層（src-tauri の command 境界）に滞留していた純粋関数を core へ寄せ、決定論テストで固める
//! （design doc 3章/4章「util」・レイヤー依存を一方向に保つ）。FS/IPC/UI 型は一切持ち込まない。

/// 昇順のバイトオフセット列を、対応する UTF-16 コードユニットオフセット列へ一括変換する（O(L) 単一パス）。
///
/// CM6（フロントのエディタ）は UTF-16 コードユニットで位置を扱うため、pika-core::search が返す
/// バイトオフセットを UTF-16 へ写す。command 境界（src-tauri）から呼ぶ。
/// `byte_offsets` は **char 境界**かつ**昇順**である前提（`search_all` のマッチ群は前方走査で昇順なので
/// `[start_0, end_0, start_1, end_1, …]` は全境界が昇順になる）。`text.char_indices()` を **1 回**走査
/// しながら「現バイト位置→累積 UTF-16 ユニット数」を進め、ソート済みの境界バイト列を順に消化する。
///
/// per-match `text[..b].encode_utf16().count()` は O(N·L)（N マッチ × L 文字長）で禁止。本実装は走査と
/// 境界消化が単調前進するため全体 O(L)（`text` の char 数）に収まる。末尾 `text.len()` の境界も正しく返す。
pub fn byte_to_utf16_offsets(text: &str, byte_offsets: &[usize]) -> Vec<usize> {
    let mut out = Vec::with_capacity(byte_offsets.len());
    let mut idx = 0usize; // 次に解決する byte_offsets のインデックス。
    let mut utf16_pos = 0usize; // 直前までに走査した char の累積 UTF-16 ユニット数。
    for (b, ch) in text.char_indices() {
        // `b` はこの char の開始バイト＝char 境界。`b` 以下の保留境界をここで確定する
        // （`utf16_pos` は byte `b` より前の全 char 分の UTF-16 ユニット数）。境界は昇順前提なので、
        // `==` だけでなく `<=` を消化して前提が崩れても出力数を保つ（防御的・無限ループ防止）。
        while idx < byte_offsets.len() && byte_offsets[idx] <= b {
            out.push(utf16_pos);
            idx += 1;
        }
        utf16_pos += ch.len_utf16();
    }
    // 末尾の境界（最終 char の直後＝`text.len()`）は走査後の累積値で確定する。
    while idx < byte_offsets.len() {
        out.push(utf16_pos);
        idx += 1;
    }
    out
}

/// バイト数を人間可読に（エラー文言用・"512.0 MB" 等）。
///
/// 1024 進数で B/KB/MB/GB を選び、小数第1位まで表示する（GB を超えても GB 止まり）。
pub fn human_bytes(n: u64) -> String {
    const UNITS: &[&str] = &["B", "KB", "MB", "GB"];
    let mut v = n as f64;
    let mut i = 0;
    while v >= 1024.0 && i < UNITS.len() - 1 {
        v /= 1024.0;
        i += 1;
    }
    format!("{v:.1} {}", UNITS[i])
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 参照実装: byte_offsets を 1 件ずつ `encode_utf16().count()` で変換する（O(N·L)）。
    /// 単一パス版の出力がこれと一致することをサロゲート混在で確認する（速度ではなく正しさの照合用）。
    fn naive(text: &str, byte_offsets: &[usize]) -> Vec<usize> {
        byte_offsets
            .iter()
            .map(|&b| text[..b].encode_utf16().count())
            .collect()
    }

    #[test]
    fn utf16変換_asciiはbyteと一致する() {
        // ASCII は 1 char = 1 byte = 1 UTF-16 ユニットなので byte と同値。
        let text = "abcdef";
        let offsets = [0usize, 1, 3, 6];
        assert_eq!(byte_to_utf16_offsets(text, &offsets), vec![0, 1, 3, 6]);
        assert_eq!(byte_to_utf16_offsets(text, &offsets), naive(text, &offsets));
    }

    #[test]
    fn utf16変換_日本語bmpは1ユニット() {
        // "あいう" は各 char utf8=3 / utf16=1。byte 0/3/6/9 → utf16 0/1/2/3。
        let text = "あいう";
        let offsets = [0usize, 3, 6, 9];
        assert_eq!(byte_to_utf16_offsets(text, &offsets), vec![0, 1, 2, 3]);
        assert_eq!(byte_to_utf16_offsets(text, &offsets), naive(text, &offsets));
    }

    #[test]
    fn utf16変換_絵文字サロゲートを複数マッチ混在で正しく写す() {
        // "a😀bb😀cc"。"😀" は utf8=4 / utf16=2（サロゲートペア）。
        // バイト配置: a=0 / 😀=1..5 / b=5 / b=6 / 😀=7..11 / c=11 / c=12 / 末尾=13。
        // "bb"（byte 5..7）を検索した想定のマッチ群を模す。1 件目 bb=5..7。
        let text = "a😀bb😀cc";
        assert_eq!(text.len(), 13); // 1 + 4 + 1 + 1 + 4 + 1 + 1
                                    // [start_0, end_0] = "bb" の byte 範囲。サロゲートを跨いだ後の位置。
        let offsets = [5usize, 7];
        let got = byte_to_utf16_offsets(text, &offsets);
        // a(1) + 😀(2) = utf16 3 で "bb" 開始、+2 で end=5。
        assert_eq!(got, vec![3, 5]);
        assert_eq!(got, naive(text, &offsets));

        // さらに "cc"（byte 11..13）も含む昇順列で、サロゲートを 2 回跨いでも一致する。
        let offsets2 = [5usize, 7, 11, 13];
        let got2 = byte_to_utf16_offsets(text, &offsets2);
        // cc 開始 utf16 = a1 + 😀2 + bb2 + 😀2 = 7、end=9。
        assert_eq!(got2, vec![3, 5, 7, 9]);
        assert_eq!(got2, naive(text, &offsets2));
    }

    #[test]
    fn utf16変換_空オフセット列と末尾境界() {
        // 空入力 → 空出力。
        assert_eq!(byte_to_utf16_offsets("abc", &[]), Vec::<usize>::new());
        // 末尾境界 text.len() を正しく返す（最終 char の直後）。
        let text = "a😀"; // byte len = 5、utf16 len = 1 + 2 = 3。
        assert_eq!(text.len(), 5);
        let offsets = [0usize, 1, 5];
        let got = byte_to_utf16_offsets(text, &offsets);
        assert_eq!(got, vec![0, 1, 3]); // a 開始=0 / 😀 開始=1 / 末尾=3。
        assert_eq!(got, naive(text, &offsets));
        // 空文字列の末尾境界（len=0）も 0 を返す。
        assert_eq!(byte_to_utf16_offsets("", &[0]), vec![0]);
    }

    #[test]
    fn search_in_textのutf16はマッチ群で一括変換と一致する() {
        // search_in_text が組む昇順バイト列（[start,end,...]）を byte_to_utf16_offsets が正しく写すこと
        // を、サロゲート混在テキストで直接確認する（command の組み立てロジックの回帰防止）。
        let text = "x😀foo😀foo";
        // "foo"（byte 5..8 と byte 12..15）の 2 マッチを想定した昇順バイト列。
        let byte_offsets = [5usize, 8, 12, 15];
        let utf16 = byte_to_utf16_offsets(text, &byte_offsets);
        // x1 + 😀2 = 3 で 1 件目開始、+3 = 6 で end。😀2 を挟み 8 で 2 件目開始、+3 = 11 で end。
        assert_eq!(utf16, vec![3, 6, 8, 11]);
        assert_eq!(utf16, naive(text, &byte_offsets));
    }

    #[test]
    fn human_bytes_は単位を繰り上げる() {
        assert_eq!(human_bytes(0), "0.0 B");
        assert_eq!(human_bytes(512), "512.0 B");
        assert_eq!(human_bytes(1024), "1.0 KB");
        assert_eq!(human_bytes(1024 * 1024), "1.0 MB");
        assert_eq!(human_bytes(10 * 1024 * 1024), "10.0 MB");
        assert_eq!(human_bytes(1024 * 1024 * 1024), "1.0 GB");
        // GB を超えても GB 止まり（UNITS の最後で打ち切る）。
        assert_eq!(human_bytes(2048u64 * 1024 * 1024 * 1024), "2048.0 GB");
    }
}
