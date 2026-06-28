//! ワークスペース列挙・名前検証の純粋ロジック（UI/Tauri/FS 非依存・cargo test の決定論ゲート対象）。
//!
//! ツリー列挙（除外判定・安定順）と新規作成名の検証は**意思決定のみ**を担う純粋関数群で、
//! 実 FS 操作（`read_dir`・`create_dir`・封じ込め canonicalize）は呼び出し側（src-tauri の薄い境界）が行う。
//! app 層（src-tauri）に滞留していた判定ロジックを core へ寄せ、決定論テストで固める（design doc 3章
//! 「command 層は薄い境界」・レイヤー依存を一方向に保つ）。

use std::cmp::Ordering;

/// 除外ディレクトリ判定（settings.toml `excluded_dirs` をツリー列挙へ配線）。
///
/// `excluded_dirs`（既定 `[".git","node_modules"]`）は**ディレクトリ名**を意味するので、
/// `is_dir == true` のときだけ判定する（同名のファイルは除外しない）。一致は Windows のパス大小無視に
/// 合わせて**大文字小文字を無視**する（`.git` も `.GIT` も同一視）。
/// MVP のため判定は直下名の完全一致のみ（glob・パス全体マッチは対象外＝design doc 15章「足さない」）。
pub fn is_excluded_dir(name: &str, is_dir: bool, excluded: &[String]) -> bool {
    is_dir && excluded.iter().any(|e| e.eq_ignore_ascii_case(name))
}

/// ツリーエントリの安定順比較（フォルダ先・名前昇順）。
///
/// `(is_dir, name)` のタプルを 2 つ受け取り、`sort_by` に渡せる [`Ordering`] を返す純粋関数。
/// 判定規則は従来の `(b.is_dir, &a.name).cmp(&(a.is_dir, &b.name))` と完全に等価:
/// 第1キーで is_dir を**降順**（`true` のフォルダが先）に、同 is_dir 内では name を**昇順**に並べる。
/// 自然順/シンボリックリンク循環検出は MVP 対象外（暫定の安定順）。
pub fn compare_tree_entries(a: (bool, &str), b: (bool, &str)) -> Ordering {
    // 元実装と同一の「左右の is_dir を入れ替えた」比較でフォルダ先・名前昇順を表現する。
    (b.0, a.1).cmp(&(a.0, b.1))
}

/// 新規作成する名前を検証する（ファイル名のみ・パス脱出や予約文字の混入を防ぐ）。
///
/// `dir` への `join` 前にこれを通し、`..`/区切り文字/Windows 予約文字/制御文字を弾く（封じ込めの一次防御。
/// 最終的な配下確認は呼び出し側の AccessControl が親 canonicalize で行う＝多層防御）。
///
/// **戻り値の型（`Result<(), String>`）について**: エラーは UI へそのまま表示される確定済み文言を運ぶ。
/// [`crate::PikaError`] でラップすると Display が `不正な引数: ` を前置し**観測文字列が変わる**ため、
/// 純粋リファクタ（外部挙動不変）の制約上、ここでは確定文言を素の `String` で返す
/// （src-tauri の command は `?` でそのまま伝播し、従来と 1 バイトも変わらない）。
pub fn verify_entry_name(name: &str) -> Result<(), String> {
    let n = name.trim();
    if n.is_empty() {
        return Err("名前を入力してください".to_string());
    }
    if n == "." || n == ".." {
        return Err("その名前は使用できません".to_string());
    }
    if n.contains('/') || n.contains('\\') {
        return Err("名前にパス区切り文字（/ \\）は使えません".to_string());
    }
    // Windows で使用不可の文字（: * ? " < > |）と制御文字を弾く。
    if n.chars()
        .any(|c| (c as u32) < 0x20 || matches!(c, ':' | '*' | '?' | '"' | '<' | '>' | '|'))
    {
        return Err("名前に使用できない文字が含まれています".to_string());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// settings.toml の既定除外リスト（`.git` / `node_modules`）。
    fn excluded() -> Vec<String> {
        vec![".git".to_string(), "node_modules".to_string()]
    }

    #[test]
    fn 既定の_git_ディレクトリは除外される() {
        assert!(is_excluded_dir(".git", true, &excluded()));
        assert!(is_excluded_dir("node_modules", true, &excluded()));
    }

    #[test]
    fn 大文字小文字を無視して一致する() {
        // Windows のパス大小無視に合わせ、.GIT も除外する。
        assert!(is_excluded_dir(".GIT", true, &excluded()));
        assert!(is_excluded_dir("Node_Modules", true, &excluded()));
    }

    #[test]
    fn 同名のファイルは除外しない() {
        // excluded_dirs はディレクトリ名の意味なので、同名ファイル（is_dir=false）は弾かない。
        assert!(!is_excluded_dir("node_modules", false, &excluded()));
        assert!(!is_excluded_dir(".git", false, &excluded()));
    }

    #[test]
    fn 除外リストに無いディレクトリは残る() {
        assert!(!is_excluded_dir("src", true, &excluded()));
        assert!(!is_excluded_dir("docs", true, &excluded()));
    }

    #[test]
    fn 空の除外リストでは何も除外しない() {
        let empty: Vec<String> = Vec::new();
        assert!(!is_excluded_dir(".git", true, &empty));
        assert!(!is_excluded_dir("node_modules", true, &empty));
    }

    #[test]
    fn 安定順はフォルダ先で名前昇順() {
        // フォルダ（is_dir=true）はファイルより前。
        assert_eq!(
            compare_tree_entries((true, "z"), (false, "a")),
            Ordering::Less
        );
        assert_eq!(
            compare_tree_entries((false, "a"), (true, "z")),
            Ordering::Greater
        );
        // 同種（共にフォルダ/共にファイル）は名前昇順。
        assert_eq!(
            compare_tree_entries((true, "a"), (true, "b")),
            Ordering::Less
        );
        assert_eq!(
            compare_tree_entries((false, "b"), (false, "a")),
            Ordering::Greater
        );
        assert_eq!(
            compare_tree_entries((false, "same"), (false, "same")),
            Ordering::Equal
        );
    }

    #[test]
    fn 安定順は元実装の_sort_by_と等価() {
        // 旧 `(b.is_dir, &a.name).cmp(&(a.is_dir, &b.name))` と同じ並びになることを実列で固定する。
        #[derive(Debug, PartialEq, Eq)]
        struct E {
            name: String,
            is_dir: bool,
        }
        let mk = |name: &str, is_dir: bool| E {
            name: name.to_string(),
            is_dir,
        };
        let mut new_sorted = vec![
            mk("zeta.txt", false),
            mk("alpha", true),
            mk("beta.txt", false),
            mk("gamma", true),
            mk("alpha.txt", false),
        ];
        let mut old_sorted = vec![
            mk("zeta.txt", false),
            mk("alpha", true),
            mk("beta.txt", false),
            mk("gamma", true),
            mk("alpha.txt", false),
        ];
        new_sorted.sort_by(|a, b| compare_tree_entries((a.is_dir, &a.name), (b.is_dir, &b.name)));
        old_sorted.sort_by(|a, b| (b.is_dir, &a.name).cmp(&(a.is_dir, &b.name)));
        assert_eq!(new_sorted, old_sorted);
        // 期待順序: フォルダ（alpha, gamma 昇順）→ ファイル（alpha.txt, beta.txt, zeta.txt 昇順）。
        let names: Vec<&str> = new_sorted.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(
            names,
            vec!["alpha", "gamma", "alpha.txt", "beta.txt", "zeta.txt"]
        );
    }

    #[test]
    fn 通常のファイル名は許可される() {
        assert!(verify_entry_name("memo.md").is_ok());
        assert!(verify_entry_name("新しいフォルダ").is_ok());
        assert!(verify_entry_name("  trimmed.txt  ").is_ok());
    }

    #[test]
    fn 空やドットのみの名前は拒否される() {
        assert!(verify_entry_name("").is_err());
        assert!(verify_entry_name("   ").is_err());
        assert!(verify_entry_name(".").is_err());
        assert!(verify_entry_name("..").is_err());
    }

    #[test]
    fn パス区切りや予約文字を含む名前は拒否される() {
        // パス脱出（区切り文字/..）を名前に混ぜられない（封じ込めの一次防御）。
        assert!(verify_entry_name("a/b.md").is_err());
        assert!(verify_entry_name("a\\b.md").is_err());
        assert!(verify_entry_name("..\\evil.md").is_err());
        // Windows 予約文字。
        assert!(verify_entry_name("a:b").is_err());
        assert!(verify_entry_name("a*b").is_err());
        assert!(verify_entry_name("a?b").is_err());
        assert!(verify_entry_name("a\"b").is_err());
        assert!(verify_entry_name("a<b").is_err());
        assert!(verify_entry_name("a>b").is_err());
        assert!(verify_entry_name("a|b").is_err());
        // 制御文字。
        assert!(verify_entry_name("a\u{0007}b").is_err());
    }

    #[test]
    fn 確定済みエラー文言は前置語なしの素の文言を保つ() {
        // PikaError ラップを避けた理由の回帰固定（外部挙動不変）: UI 表示文言が 1 バイトも変わらない。
        assert_eq!(verify_entry_name("").unwrap_err(), "名前を入力してください");
        assert_eq!(
            verify_entry_name(".").unwrap_err(),
            "その名前は使用できません"
        );
        assert_eq!(
            verify_entry_name("a/b").unwrap_err(),
            "名前にパス区切り文字（/ \\）は使えません"
        );
        assert_eq!(
            verify_entry_name("a:b").unwrap_err(),
            "名前に使用できない文字が含まれています"
        );
    }
}
