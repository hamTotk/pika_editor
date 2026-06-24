//! 内容ハッシュの共通規則（自己保存抑制・未読判定・復元の別物判定で同一規則を使う）。
//!
//! 改行のみの差を未読/差分に出さない方針（要件8.1）と整合させるため、
//! ハッシュは**内容を LF 正規化してから** XxHash64（seed=0）で取る。
//! 自己保存抑制（commands.rs）・state.json 復元の別物判定（state_store.rs）・
//! state.json 保存時のタブ content_hash（hash_content command）が**同じ値**になることを
//! 一箇所に集約して保証する（以前は commands.rs / state_store.rs に重複実装され
//! 「同一規則」をコメントで約束していた＝ドリフト源だった）。

use std::hash::Hasher;

/// 巨大ファイル第1段階の閾値（要件2.2・sprint6 CM6 実測で 10MB に確定）。
/// この値以上のファイルはハッシュのみ記録し、起動復元時の content_hash 全量読込もしない
/// （起動0.5秒ゲートのホットパス保護＝spec「10MB 以上はハッシュのみ」）。
/// [`crate::huge::STAGE1_THRESHOLD_BYTES`]・[`crate::snapshot::policy::DEFAULT_CONTENT_LIMIT_BYTES`] と同値。
pub const HUGE_FILE_THRESHOLD_BYTES: u64 = 10 * 1024 * 1024;

/// バイト列を LF 正規化して返す（`CRLF`/`CR` → `LF`）。本クレートの LF 正規化の単一実装。
///
/// 改行のみの差を未読/差分に出さない方針（要件8.1）を、ハッシュ・差分照合・退避 object の
/// アドレッシングで**同一ルーチン**に集約する（#40・以前は 3 箇所で別実装＝ドリフト源だった）。
/// CR/LF は ASCII で UTF-8 継続バイト（0x80-0xBF）と衝突しないため、バイト単位の正規化でも
/// 入力が妥当な UTF-8 なら出力の UTF-8 妥当性は保たれる（`diff::line` の `&str` 委譲が成立する根拠）。
pub fn normalize_lf_bytes(bytes: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\r' => {
                out.push(b'\n');
                // CRLF はまとめて 1 つの LF にする（CR 単独は LF へ）。
                if i + 1 < bytes.len() && bytes[i + 1] == b'\n' {
                    i += 1;
                }
            }
            b => out.push(b),
        }
        i += 1;
    }
    out
}

/// バイト列を LF 正規化してハッシュ化する（`CRLF`/`CR` → `LF`）。
///
/// 戻りは 16 桁の小文字 16 進文字列（XxHash64・seed=0）。
/// 自己保存抑制トークン・復元時の別物判定・タブ content_hash で**必ず本関数を使う**こと。
pub fn hash_normalized_lf(bytes: &[u8]) -> String {
    let out = normalize_lf_bytes(bytes);
    let mut h = twox_hash::XxHash64::with_seed(0);
    h.write(&out);
    format!("{:016x}", h.finish())
}

/// 文字列を LF 正規化してハッシュ化する（[`hash_normalized_lf`] の `&str` 版）。
pub fn hash_normalized_lf_str(content: &str) -> String {
    hash_normalized_lf(content.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crlf_と_lf_は同じハッシュになる() {
        // 改行のみの差は未読/差分に出さない（要件8.1）。
        let lf = hash_normalized_lf(b"a\nb\nc");
        let crlf = hash_normalized_lf(b"a\r\nb\r\nc");
        let cr = hash_normalized_lf(b"a\rb\rc");
        assert_eq!(lf, crlf);
        assert_eq!(lf, cr);
    }

    #[test]
    fn 内容が違えばハッシュも違う() {
        assert_ne!(hash_normalized_lf(b"abc"), hash_normalized_lf(b"abd"));
    }

    #[test]
    fn 空入力でも決定論的() {
        assert_eq!(hash_normalized_lf(b""), hash_normalized_lf(b""));
        assert_eq!(hash_normalized_lf(b"").len(), 16);
    }

    #[test]
    fn str_版とバイト版が一致する() {
        assert_eq!(
            hash_normalized_lf_str("hello\r\nworld"),
            hash_normalized_lf(b"hello\r\nworld")
        );
    }

    #[test]
    fn 戻りは_16_桁の小文字_16_進() {
        let h = hash_normalized_lf(b"some content");
        assert_eq!(h.len(), 16);
        assert!(h
            .chars()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase()));
    }

    #[test]
    fn 巨大ファイル閾値は_10mb() {
        assert_eq!(HUGE_FILE_THRESHOLD_BYTES, 10 * 1024 * 1024);
    }
}
