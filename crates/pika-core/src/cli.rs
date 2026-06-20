//! CLI 引数の純粋パースロジック（要件3.1・design doc 9章）。
//!
//! 本モジュールは I/O を行わない純粋関数のみを置く（cargo test の決定論ゲート対象）。
//! `pika-cli` クレートが本ロジックを呼び、GUI 起動判断や名前付きパイプ転送に使う。
//!
//! `-g <file>[:<行>[:<桁>]]`（VS Code 互換）のパース規則の核心は
//! **先頭ドライブレター直後のコロンを行・桁の区切りとして扱わない**こと
//! （`C:\dir\a.md:12:3` の最初の `:` は分割対象外。末尾から行・桁を剥がす）。

use crate::error::{PikaError, Result};

/// `-g` で指定された「ファイル＋カーソル位置」の解析結果。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GotoTarget {
    /// ファイルパス文字列（正規化はクライアント側で別途行う。ここでは分解のみ）。
    pub file: String,
    /// 1 始まりの行番号。`-g` に行が無い場合は `None`。
    pub line: Option<u32>,
    /// 1 始まりの桁番号。桁省略時は `None`（呼び出し側で行頭=1 扱い）。
    pub column: Option<u32>,
}

/// `-g <spec>` の `<spec>` 部分を解析する。
///
/// 分割規則（要件3.1）:
/// - 末尾から最大 2 個の `:<整数>` を「桁」「行」として剥がす。
/// - 先頭が `X:`（ドライブレター + コロン）の場合、その `:` は**ファイルパスの一部**として残す。
/// - 桁省略（`a.md:12`）は行頭扱いの `None`。非整数の末尾セグメントは位置として採用しない
///   （`a.md:foo` は行・桁を無視しファイル名 `a.md:foo`… ではなく、整数でないため剥がさない）。
///
/// 非整数・行のみ・桁のみのケースは「整数でなければ剥がさない」一貫規則で扱う。
pub fn parse_goto_spec(spec: &str) -> Result<GotoTarget> {
    if spec.is_empty() {
        return Err(PikaError::InvalidArgument("-g の引数が空".into()));
    }

    // ドライブレター接頭辞（例 "C:"）の終端バイト位置。これより手前の `:` は分割しない。
    let drive_prefix_len = drive_prefix_len(spec);

    // 末尾から「`:<整数>`」を最大 2 回剥がす。剥がした順に column, line。
    let mut rest = spec;
    let mut tail_numbers: Vec<u32> = Vec::new();

    for _ in 0..2 {
        match split_trailing_colon_number(rest, drive_prefix_len) {
            Some((head, num)) => {
                tail_numbers.push(num);
                rest = head;
            }
            None => break,
        }
    }

    // tail_numbers は末尾から積んだので [column?, line?] の順。逆順で line, column。
    let (line, column) = match tail_numbers.len() {
        0 => (None, None),
        1 => (Some(tail_numbers[0]), None),
        _ => (Some(tail_numbers[1]), Some(tail_numbers[0])),
    };

    if rest.is_empty() {
        return Err(PikaError::InvalidArgument(
            "-g にファイルパスが含まれない".into(),
        ));
    }

    Ok(GotoTarget {
        file: rest.to_string(),
        line,
        column,
    })
}

/// 先頭の `X:`（A-Z / a-z + コロン）を検出し、コロンまでのバイト長を返す。無ければ 0。
/// この長さより手前の `:` は行・桁の区切りとして剥がさない（ドライブレター保護）。
fn drive_prefix_len(spec: &str) -> usize {
    let bytes = spec.as_bytes();
    if bytes.len() >= 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':' {
        2
    } else {
        0
    }
}

/// `rest` の末尾が `:<整数>` ならそれを剥がし `(head, 整数)` を返す。
/// ただしコロン位置が `protect_until` 未満（ドライブレターのコロン）なら剥がさない。
fn split_trailing_colon_number(rest: &str, protect_until: usize) -> Option<(&str, u32)> {
    let colon_idx = rest.rfind(':')?;
    // ドライブレターのコロンは保護（`C:` の `:` を分割しない）。
    if colon_idx < protect_until {
        return None;
    }
    let num_part = &rest[colon_idx + 1..];
    if num_part.is_empty() {
        return None;
    }
    // 整数（1 始まりの 10 進数）でなければ位置として採用しない。
    let num: u32 = num_part.parse().ok()?;
    Some((&rest[..colon_idx], num))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ドライブレターのコロンを分割しない() {
        // C:\... の最初のコロンはパスの一部。行・桁のみ末尾から剥がす。
        let t = parse_goto_spec(r"C:\dir\a.md:12:3").unwrap();
        assert_eq!(t.file, r"C:\dir\a.md");
        assert_eq!(t.line, Some(12));
        assert_eq!(t.column, Some(3));
    }

    #[test]
    fn ドライブレター小文字でも保護する() {
        let t = parse_goto_spec(r"d:\work\note.md:7").unwrap();
        assert_eq!(t.file, r"d:\work\note.md");
        assert_eq!(t.line, Some(7));
        assert_eq!(t.column, None);
    }

    #[test]
    fn 行のみ指定は桁が_none() {
        let t = parse_goto_spec("a.md:12").unwrap();
        assert_eq!(t.file, "a.md");
        assert_eq!(t.line, Some(12));
        assert_eq!(t.column, None);
    }

    #[test]
    fn 位置指定なしはファイルのみ() {
        let t = parse_goto_spec("README.md").unwrap();
        assert_eq!(t.file, "README.md");
        assert_eq!(t.line, None);
        assert_eq!(t.column, None);
    }

    #[test]
    fn 非整数の末尾セグメントは位置にしない() {
        // a.md:foo の :foo は整数でないため剥がさず、パスの一部として残す。
        let t = parse_goto_spec("a.md:foo").unwrap();
        assert_eq!(t.file, "a.md:foo");
        assert_eq!(t.line, None);
        assert_eq!(t.column, None);
    }

    #[test]
    fn ドライブレターのみで位置なし() {
        let t = parse_goto_spec(r"C:\a.md").unwrap();
        assert_eq!(t.file, r"C:\a.md");
        assert_eq!(t.line, None);
        assert_eq!(t.column, None);
    }

    #[test]
    fn ドライブレター直後のコロンだけでは剥がさない() {
        // "C:" 単体は protect_until=2 でコロンが保護され、行として剥がれない。
        let t = parse_goto_spec("C:").unwrap();
        assert_eq!(t.file, "C:");
        assert_eq!(t.line, None);
        assert_eq!(t.column, None);
    }

    #[test]
    fn 空引数はエラー() {
        assert!(parse_goto_spec("").is_err());
    }

    #[test]
    fn ドライブレターのコロンを保護しつつ桁省略() {
        let t = parse_goto_spec(r"C:\dir\a.md:99").unwrap();
        assert_eq!(t.file, r"C:\dir\a.md");
        assert_eq!(t.line, Some(99));
        assert_eq!(t.column, None);
    }
}
