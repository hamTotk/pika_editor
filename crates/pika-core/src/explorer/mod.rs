//! エクスプローラー統合の登録エントリ（要件3.3・純粋ロジック）。
//!
//! 本モジュールは OS 非依存の純粋ロジックのみを置く（cargo test の決定論ゲート対象）。
//! 実際のレジストリ書込/削除（`Reg*`）と `SHChangeNotify` は `pika-cli`／`src-tauri` の
//! 薄いラッパが行う（design doc 3章「OS 呼び出しは境界に閉じる」）。
//!
//! 設計（要件3.3 エクスプローラー統合）:
//! - 登録先は `HKCU\Software\Classes`（管理者不要・ユーザー単位。ポータブル版は自己登録しない方針）。
//! - `.md/.markdown/.html/.htm` の `OpenWithProgids` に ProgId [`PROG_ID`] を **追加** する
//!   （「プログラムから開く」候補に出すだけ＝拡張子の既定値は上書きしない。値追加のみ）。
//! - ファイル/フォルダ右クリックに「pikaで開く」を足す（`*\shell\pika`・`Directory\shell\pika`）。
//! - **Windows 11 は既定アプリをアプリ側から強制設定できない**（`UserChoice` がハッシュ保護）。
//!   候補登録＋右クリックまでが限界で、最後はユーザーが手動で「常にこのアプリで開く」を選ぶ
//!   （要件3.3 の設計どおり）。

/// HKCU 配下へ書き込む 1 レジストリ値（`key` は `HKEY_CURRENT_USER` からの相対キーパス）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RegEntry {
    /// `HKEY_CURRENT_USER` からの相対キーパス（例 `Software\Classes\pika.AssocFile`）。
    pub key: String,
    /// 値名（空文字 `""` = キーの既定値）。
    pub name: String,
    /// 値データ（`REG_SZ` 文字列）。`OpenWithProgids` の候補値はデータ空が慣例。
    pub data: String,
}

/// 関連付ける拡張子（要件3.3）。先頭ドット込み。
pub const ASSOC_EXTENSIONS: &[&str] = &[".md", ".markdown", ".html", ".htm"];

/// pika の ProgId。拡張子の `OpenWithProgids` 値名・ProgId キー名・右クリック表示の根。
pub const PROG_ID: &str = "pika.AssocFile";

/// ProgId 本体キー（`Software\Classes\pika.AssocFile`）。
fn progid_key() -> String {
    format!("Software\\Classes\\{PROG_ID}")
}

/// 全ファイル右クリックの「pikaで開く」キー（`Software\Classes\*\shell\pika`）。
fn file_verb_key() -> String {
    "Software\\Classes\\*\\shell\\pika".to_string()
}

/// フォルダ右クリックの「pikaで開く」キー（`Software\Classes\Directory\shell\pika`）。
fn dir_verb_key() -> String {
    "Software\\Classes\\Directory\\shell\\pika".to_string()
}

/// 指定拡張子の `OpenWithProgids` キー（`Software\Classes\<ext>\OpenWithProgids`）。
fn open_with_progids_key(ext: &str) -> String {
    format!("Software\\Classes\\{ext}\\OpenWithProgids")
}

/// 登録で書き込む [`RegEntry`] 群を返す（`exe_path` = `pika.exe` の絶対パス）。
///
/// command は `"<exe>" "%1"`（exe を必ずダブルクオート＝空白入りパス対応・`%1` で開く対象を渡す）。
/// DefaultIcon は `"<exe>",0`（exe の 0 番アイコン）。`OpenWithProgids` は**値追加のみ**で
/// 拡張子の既定値（既定アプリ）は一切触らない（要件3.3「候補登録」）。
pub fn registration_entries(exe_path: &str) -> Vec<RegEntry> {
    let command = format!("\"{exe_path}\" \"%1\"");
    let icon = format!("\"{exe_path}\",0");
    let progid = progid_key();
    let mut entries = Vec::new();

    // ProgId 本体（候補に出す表示名・アイコン・open コマンド）。
    entries.push(RegEntry {
        key: progid.clone(),
        name: String::new(),
        data: "Pika Editor ドキュメント".to_string(),
    });
    entries.push(RegEntry {
        key: format!("{progid}\\DefaultIcon"),
        name: String::new(),
        data: icon.clone(),
    });
    entries.push(RegEntry {
        key: format!("{progid}\\shell\\open\\command"),
        name: String::new(),
        data: command.clone(),
    });

    // 各拡張子の OpenWithProgids に ProgId を**追加**する（既定値は壊さない＝候補登録のみ）。
    for ext in ASSOC_EXTENSIONS {
        entries.push(RegEntry {
            key: open_with_progids_key(ext),
            name: PROG_ID.to_string(),
            // OpenWithProgids の候補値はデータ空（REG_SZ の空文字）が Windows の慣例。
            data: String::new(),
        });
    }

    // 右クリック「pikaで開く」（全ファイル / フォルダ）。表示名・アイコン・command を置く。
    for verb in [file_verb_key(), dir_verb_key()] {
        entries.push(RegEntry {
            key: verb.clone(),
            name: String::new(),
            data: "Pika Editorで開く".to_string(),
        });
        entries.push(RegEntry {
            key: verb.clone(),
            name: "Icon".to_string(),
            data: icon.clone(),
        });
        entries.push(RegEntry {
            key: format!("{verb}\\command"),
            name: String::new(),
            data: command.clone(),
        });
    }

    entries
}

/// 解除（アンインストール）の対象を返す。
///
/// - `.0` = `RegDeleteTree` でキーごと消す独自キー群（ProgId 本体・右クリック2種）。
/// - `.1` = `(キー, 削除する値名)`。各拡張子の `OpenWithProgids` から **pika の値だけ** 消す
///   （拡張子キー本体・`OpenWithProgids` キー自体は残す＝他アプリの候補を壊さない）。
pub fn unregistration() -> (Vec<String>, Vec<(String, String)>) {
    let trees = vec![progid_key(), file_verb_key(), dir_verb_key()];
    let values = ASSOC_EXTENSIONS
        .iter()
        .map(|ext| (open_with_progids_key(ext), PROG_ID.to_string()))
        .collect();
    (trees, values)
}

/// 値削除（[`unregistration`] の `.1`）の後に「**空のときだけ**消す」候補キーを子→親順で返す。
///
/// register は新規プロファイルで `Software\Classes\<ext>\OpenWithProgids`（と途中の `<ext>` キー）を
/// 自分で作りうる。値だけ消すと空キーが残り「残骸ゼロ」に反するため、これらを後始末する。ただし
/// **他アプリの値やサブキーが残るキーは消さない**判断が要るので、ここでは候補キーを列挙するだけにし、
/// 実際の「空判定→削除」は OS 側ラッパ（`pika-cli` の `win::delete_key_if_empty`）が担う（design doc 3章）。
///
/// 順序が肝心: 各拡張子で `OpenWithProgids`（子）を先に、`<ext>` 本体（親）を後に置く。子が消えてから
/// 親の空判定が成立する（親は子を持つ間は「空でない」と判定され温存される）。
pub fn empty_delete_candidates() -> Vec<String> {
    let mut keys = Vec::with_capacity(ASSOC_EXTENSIONS.len() * 2);
    for ext in ASSOC_EXTENSIONS {
        keys.push(open_with_progids_key(ext)); // 子（先に空判定・削除）
        keys.push(format!("Software\\Classes\\{ext}")); // 親（子が消えてから判定）
    }
    keys
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXE: &str = r"C:\Program Files\pika\pika.exe";

    #[test]
    fn 登録は四拡張子すべての_openwithprogids_に_progid_を値追加する() {
        let entries = registration_entries(EXE);
        // 要件3.3 の対象は .md/.markdown/.html/.htm の 4 つ。
        assert_eq!(ASSOC_EXTENSIONS.len(), 4);
        for ext in ASSOC_EXTENSIONS {
            let key = open_with_progids_key(ext);
            let found = entries
                .iter()
                .any(|e| e.key == key && e.name == PROG_ID && e.data.is_empty());
            assert!(found, "{ext} の OpenWithProgids に {PROG_ID} 値（データ空）が無い");
            // 既定値（name=""）は触らない＝候補登録のみ（既定アプリを壊さない）。
            let touches_default = entries.iter().any(|e| e.key == key && e.name.is_empty());
            assert!(
                !touches_default,
                "{ext} の OpenWithProgids 既定値を上書きしている（候補登録のはず）"
            );
        }
    }

    #[test]
    fn 登録の_command_は_exe_をダブルクオートし_percent1_を渡す() {
        let entries = registration_entries(EXE);
        let expected = format!("\"{EXE}\" \"%1\"");
        // ProgId の open command と 右クリック2種の command がすべて同形であること。
        let command_keys = [
            format!("Software\\Classes\\{PROG_ID}\\shell\\open\\command"),
            "Software\\Classes\\*\\shell\\pika\\command".to_string(),
            "Software\\Classes\\Directory\\shell\\pika\\command".to_string(),
        ];
        for key in command_keys {
            let e = entries
                .iter()
                .find(|e| e.key == key && e.name.is_empty())
                .unwrap_or_else(|| panic!("command キーが無い: {key}"));
            assert_eq!(e.data, expected, "{key} の command が想定と異なる");
        }
    }

    #[test]
    fn 登録は_progid_表示名と右クリック表示名を持つ() {
        let entries = registration_entries(EXE);
        // ProgId 既定値（候補に出る表示名）。
        assert!(entries.iter().any(|e| e.key
            == format!("Software\\Classes\\{PROG_ID}")
            && e.name.is_empty()
            && !e.data.is_empty()));
        // 右クリック「pikaで開く」（ファイル/フォルダ）の表示名。
        for verb in [
            "Software\\Classes\\*\\shell\\pika",
            "Software\\Classes\\Directory\\shell\\pika",
        ] {
            assert!(
                entries
                    .iter()
                    .any(|e| e.key == verb && e.name.is_empty() && e.data == "Pika Editorで開く"),
                "{verb} の右クリック表示名が無い"
            );
        }
    }

    #[test]
    fn 解除は独自キーを_deletetree_対象にする() {
        let (trees, _values) = unregistration();
        assert!(trees.contains(&format!("Software\\Classes\\{PROG_ID}")));
        assert!(trees.contains(&"Software\\Classes\\*\\shell\\pika".to_string()));
        assert!(trees.contains(&"Software\\Classes\\Directory\\shell\\pika".to_string()));
    }

    #[test]
    fn 解除は拡張子キーを消さず_openwithprogids_の_pika_値だけ消す() {
        let (trees, values) = unregistration();
        for ext in ASSOC_EXTENSIONS {
            let owp = open_with_progids_key(ext);
            // 値削除リストに (OpenWithProgids, PROG_ID) があること。
            assert!(
                values.iter().any(|(k, v)| k == &owp && v == PROG_ID),
                "{ext} の OpenWithProgids から {PROG_ID} 値を消す指定が無い"
            );
            // 拡張子キー本体・OpenWithProgids キー自体は DeleteTree しない（他アプリの候補を壊さない）。
            assert!(
                !trees
                    .iter()
                    .any(|t| t == &format!("Software\\Classes\\{ext}") || t == &owp),
                "{ext} を丸ごと削除している（候補登録の解除は値削除のみのはず）"
            );
        }
    }

    #[test]
    fn 空キー掃除候補は各拡張子で子owpを親extより先に置く() {
        let cands = empty_delete_candidates();
        // 各拡張子につき OpenWithProgids（子）と <ext> 本体（親）の 2 つ。
        assert_eq!(cands.len(), ASSOC_EXTENSIONS.len() * 2);
        for ext in ASSOC_EXTENSIONS {
            let owp = open_with_progids_key(ext);
            let ext_key = format!("Software\\Classes\\{ext}");
            let owp_at = cands.iter().position(|k| k == &owp);
            let ext_at = cands.iter().position(|k| k == &ext_key);
            assert!(owp_at.is_some(), "{ext} の OpenWithProgids 掃除候補が無い");
            assert!(ext_at.is_some(), "{ext} 本体の掃除候補が無い");
            // 親（<ext>）は子（OpenWithProgids）より後ろ＝子を消してから親の空判定が成立する。
            assert!(
                owp_at.unwrap() < ext_at.unwrap(),
                "{ext}: OpenWithProgids（子）を <ext>（親）より先に掃除していない"
            );
        }
    }

    #[test]
    fn 登録と解除の独自キーが対応する() {
        let entries = registration_entries(EXE);
        let (trees, _values) = unregistration();
        // DeleteTree 対象の各キーは、登録エントリのいずれか（自身 or 配下）に対応すること（取りこぼし防止）。
        for t in &trees {
            let covered = entries
                .iter()
                .any(|e| e.key == *t || e.key.starts_with(&format!("{t}\\")));
            assert!(covered, "解除対象 {t} に対応する登録エントリが無い");
        }
    }
}
