//! エクスプローラー統合の登録エントリ（要件3.3・純粋ロジック）。
//!
//! 本モジュールは OS 非依存の純粋ロジックのみを置く（cargo test の決定論ゲート対象）。
//! 実際のレジストリ書込/削除（`Reg*`）と `SHChangeNotify` は `pika-cli`／`src-tauri` の
//! 薄いラッパが行う（design doc 3章「OS 呼び出しは境界に閉じる」）。
//!
//! 設計（要件3.3 エクスプローラー統合）:
//! - 登録先は `HKCU\Software\Classes`（管理者不要・ユーザー単位。ポータブル版は自己登録しない方針）。
//! - **ファイル種別ごとに ProgId を分ける**（`pika.Markdown`＝.md/.markdown・`pika.Html`＝.html/.htm）。
//!   各 ProgId に種別専用の `DefaultIcon` を割り当てることで、既定アプリ化したファイルが
//!   エクスプローラー上で種別ごとに別アイコンになる（要件 line 222「.markdown は .md と、.htm は
//!   .html と同等に扱う」＝この 2 グループ分割と一致）。種類を増やすときは [`ASSOC_TYPES`] に
//!   1 エントリ足すだけでよい（アイコンは `src-tauri/icons/shell/generate.py` で生成）。
//! - 各拡張子の `OpenWithProgids` に対応 ProgId を **追加** する（「プログラムから開く」候補に
//!   出すだけ＝拡張子の既定値は上書きしない。値追加のみ）。
//! - ファイル/フォルダ右クリックに「pikaで開く」を足す（`*\shell\pika`・`Directory\shell\pika`）。
//! - **Windows 11 は既定アプリをアプリ側から強制設定できない**（`UserChoice` がハッシュ保護）。
//!   候補登録＋右クリックまでが限界で、最後はユーザーが手動で「常にこのアプリで開く」を選ぶ
//!   （要件3.3 の設計どおり）。

/// HKCU 配下へ書き込む 1 レジストリ値（`key` は `HKEY_CURRENT_USER` からの相対キーパス）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RegEntry {
    /// `HKEY_CURRENT_USER` からの相対キーパス（例 `Software\Classes\pika.Markdown`）。
    pub key: String,
    /// 値名（空文字 `""` = キーの既定値）。
    pub name: String,
    /// 値データ（`REG_SZ` 文字列）。`OpenWithProgids` の候補値はデータ空が慣例。
    pub data: String,
}

/// 関連付ける 1 ファイル種別の定義（ProgId・表示名・アイコン・対象拡張子）。
///
/// 種類を増やすときはここに 1 エントリ足し、合わせて
/// `src-tauri/icons/shell/generate.py` の `TYPES`（同じ `icon_file`）と
/// `src-tauri/tauri.bundle.conf.json` の `bundle.resources`（.ico 同梱）を更新する。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AssocType {
    /// ProgId（`Software\Classes\<progid>`。`OpenWithProgids` の値名にもなる）。
    pub progid: &'static str,
    /// 「プログラムから開く」候補や既定アプリ欄に出る表示名（ProgId 既定値）。
    pub display: &'static str,
    /// 種別アイコンのファイル名（exe の隣に同梱される。`DefaultIcon` が指す）。
    pub icon_file: &'static str,
    /// この種別に割り当てる拡張子（先頭ドット込み）。
    pub extensions: &'static [&'static str],
}

/// 関連付ける種別テーブル（要件3.3・line 222 のグループ分けに一致）。
pub const ASSOC_TYPES: &[AssocType] = &[
    AssocType {
        progid: "pika.Markdown",
        display: "Pika Editor Markdown ドキュメント",
        icon_file: "md.ico",
        extensions: &[".md", ".markdown"],
    },
    AssocType {
        progid: "pika.Html",
        display: "Pika Editor HTML ドキュメント",
        icon_file: "html.ico",
        extensions: &[".html", ".htm"],
    },
];

/// 旧バージョンが使っていた単一 ProgId。アップグレード/再登録時に残骸を残さないよう、
/// 解除（[`unregistration`]）でこれらも掃除対象に含める（書き込みは新 [`ASSOC_TYPES`] のみ）。
pub const LEGACY_PROG_IDS: &[&str] = &["pika.AssocFile"];

/// 旧バージョンが [`LEGACY_PROG_IDS`] を登録していた拡張子。掃除はこの**旧**集合を基準にする
/// （現行 [`ASSOC_TYPES`] からいつか拡張子を外しても、外した拡張子に残る旧値を取りこぼさない）。
pub const LEGACY_EXTENSIONS: &[&str] = &[".md", ".markdown", ".html", ".htm"];

/// 全 [`ASSOC_TYPES`] の拡張子を平坦化して返す（重複は想定しない）。
pub fn assoc_extensions() -> Vec<&'static str> {
    ASSOC_TYPES.iter().flat_map(|t| t.extensions.iter().copied()).collect()
}

/// ProgId 本体キー（`Software\Classes\<progid>`）。
fn progid_key(progid: &str) -> String {
    format!("Software\\Classes\\{progid}")
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

/// `DefaultIcon` 値データを組み立てる。
///
/// 種別アイコンの同梱ディレクトリ `icon_dir` が分かれば `"<dir>\<icon_file>",0`、
/// 不明（dev ビルド等で .ico が見つからない）なら exe 埋め込みアプリアイコン `"<exe>",0`
/// にフォールバックする（登録自体は必ず成立させ、見た目だけ既定ロゴに退避する）。
fn default_icon(exe_path: &str, icon_dir: Option<&str>, icon_file: &str) -> String {
    match icon_dir {
        Some(dir) if !dir.is_empty() => {
            // 末尾の区切りを正規化して二重バックスラッシュ（`C:\\md.ico`）を防ぐ。
            let dir = dir.trim_end_matches('\\');
            format!("\"{dir}\\{icon_file}\",0")
        }
        _ => format!("\"{exe_path}\",0"),
    }
}

/// 登録で書き込む [`RegEntry`] 群を返す。
///
/// - `exe_path` = `pika.exe` の絶対パス。command は `"<exe>" "%1"`（exe をダブルクオート＝
///   空白入りパス対応・`%1` で開く対象を渡す）。
/// - `icon_dir` = 種別アイコン（.ico）が置かれたディレクトリ（exe の隣に同梱される）。
///   `None`／空なら `DefaultIcon` は exe のアプリアイコンへフォールバックする。
///
/// `OpenWithProgids` は**値追加のみ**で拡張子の既定値（既定アプリ）は一切触らない（候補登録）。
pub fn registration_entries(exe_path: &str, icon_dir: Option<&str>) -> Vec<RegEntry> {
    let command = format!("\"{exe_path}\" \"%1\"");
    let app_icon = format!("\"{exe_path}\",0"); // 右クリック「pikaで開く」は汎用なのでアプリアイコン。
    let mut entries = Vec::new();

    for t in ASSOC_TYPES {
        let progid = progid_key(t.progid);
        // ProgId 本体（候補に出る表示名）。
        entries.push(RegEntry {
            key: progid.clone(),
            name: String::new(),
            data: t.display.to_string(),
        });
        // 種別専用アイコン。
        entries.push(RegEntry {
            key: format!("{progid}\\DefaultIcon"),
            name: String::new(),
            data: default_icon(exe_path, icon_dir, t.icon_file),
        });
        // open コマンド。
        entries.push(RegEntry {
            key: format!("{progid}\\shell\\open\\command"),
            name: String::new(),
            data: command.clone(),
        });
        // 各拡張子の OpenWithProgids にこの ProgId を**追加**（既定値は壊さない＝候補登録のみ）。
        for ext in t.extensions {
            entries.push(RegEntry {
                key: open_with_progids_key(ext),
                name: t.progid.to_string(),
                // OpenWithProgids の候補値はデータ空（REG_SZ の空文字）が Windows の慣例。
                data: String::new(),
            });
        }
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
            data: app_icon.clone(),
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
/// - `.0` = `RegDeleteTree` でキーごと消す独自キー群（各種別 ProgId・旧 ProgId・右クリック2種）。
/// - `.1` = `(キー, 削除する値名)`。各拡張子の `OpenWithProgids` から **pika の値だけ** 消す
///   （拡張子キー本体・`OpenWithProgids` キー自体は残す＝他アプリの候補を壊さない）。新 ProgId に
///   加え、旧 ProgId（[`LEGACY_PROG_IDS`]・かつて全拡張子へ登録）も全拡張子から消す。
pub fn unregistration() -> (Vec<String>, Vec<(String, String)>) {
    let mut trees: Vec<String> = ASSOC_TYPES.iter().map(|t| progid_key(t.progid)).collect();
    trees.extend(LEGACY_PROG_IDS.iter().map(|p| progid_key(p)));
    trees.push(file_verb_key());
    trees.push(dir_verb_key());

    let mut values = Vec::new();
    // 新 ProgId は「その種別の拡張子」から消す。
    for t in ASSOC_TYPES {
        for ext in t.extensions {
            values.push((open_with_progids_key(ext), t.progid.to_string()));
        }
    }
    // 旧 ProgId は旧版が登録した拡張子（LEGACY_EXTENSIONS）から消す（現行テーブルに依存しない）。
    for ext in LEGACY_EXTENSIONS {
        for legacy in LEGACY_PROG_IDS {
            values.push((open_with_progids_key(ext), (*legacy).to_string()));
        }
    }
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
    let exts = assoc_extensions();
    let mut keys = Vec::with_capacity(exts.len() * 2);
    for ext in exts {
        keys.push(open_with_progids_key(ext)); // 子（先に空判定・削除）
        keys.push(format!("Software\\Classes\\{ext}")); // 親（子が消えてから判定）
    }
    keys
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    const EXE: &str = r"C:\Program Files\pika\pika.exe";
    const ICONS: &str = r"C:\Program Files\pika";

    #[test]
    fn テーブルは拡張子_progid_icon_file_がそれぞれ一意() {
        // 「種類を増やすときは ASSOC_TYPES に 1 エントリ足すだけ」の不変条件を件数非依存で守る。
        // 拡張子重複は OpenWithProgids 複数値・掃除候補重複を、icon_file 重複は別種別が同じ
        // アイコンになる退化を招く。
        let exts: Vec<_> = assoc_extensions();
        assert_eq!(
            exts.iter().collect::<HashSet<_>>().len(),
            exts.len(),
            "拡張子が重複している"
        );
        let progids: Vec<_> = ASSOC_TYPES.iter().map(|t| t.progid).collect();
        assert_eq!(
            progids.iter().collect::<HashSet<_>>().len(),
            progids.len(),
            "ProgId が重複している"
        );
        let icons: Vec<_> = ASSOC_TYPES.iter().map(|t| t.icon_file).collect();
        assert_eq!(
            icons.iter().collect::<HashSet<_>>().len(),
            icons.len(),
            "icon_file が重複している"
        );
    }

    #[test]
    fn 登録は各種別の拡張子の_openwithprogids_に対応_progid_を値追加する() {
        let entries = registration_entries(EXE, Some(ICONS));
        for t in ASSOC_TYPES {
            for ext in t.extensions {
                let key = open_with_progids_key(ext);
                let found = entries
                    .iter()
                    .any(|e| e.key == key && e.name == t.progid && e.data.is_empty());
                assert!(found, "{ext} の OpenWithProgids に {} 値（データ空）が無い", t.progid);
                // 既定値（name=""）は触らない＝候補登録のみ（既定アプリを壊さない）。
                let touches_default = entries.iter().any(|e| e.key == key && e.name.is_empty());
                assert!(
                    !touches_default,
                    "{ext} の OpenWithProgids 既定値を上書きしている（候補登録のはず）"
                );
            }
        }
    }

    #[test]
    fn 種別ごとに専用_defaulticon_を持ち拡張子のグルーピングが要件と一致する() {
        let entries = registration_entries(EXE, Some(ICONS));
        // .md/.markdown は Markdown、.html/.htm は Html に割り当て（要件 line 222）。
        let md = ASSOC_TYPES.iter().find(|t| t.progid == "pika.Markdown").unwrap();
        let html = ASSOC_TYPES.iter().find(|t| t.progid == "pika.Html").unwrap();
        assert_eq!(md.extensions, &[".md", ".markdown"]);
        assert_eq!(html.extensions, &[".html", ".htm"]);
        for t in ASSOC_TYPES {
            let key = format!("Software\\Classes\\{}\\DefaultIcon", t.progid);
            let e = entries
                .iter()
                .find(|e| e.key == key && e.name.is_empty())
                .unwrap_or_else(|| panic!("{} の DefaultIcon が無い", t.progid));
            // 種別ごとに別 .ico を指す（VSCode 風に種別で別アイコン）。一意性は別テストで担保。
            assert_eq!(e.data, format!("\"{ICONS}\\{}\",0", t.icon_file));
        }
    }

    #[test]
    fn default_icon_は末尾区切り付きの_icon_dir_でも二重バックスラッシュにならない() {
        // resolve_icon_dir は通常末尾区切り無しだが、ドライブ直下等の保険。
        let entries = registration_entries(EXE, Some(r"C:\Program Files\pika\"));
        let md = ASSOC_TYPES.iter().find(|t| t.progid == "pika.Markdown").unwrap();
        let key = format!("Software\\Classes\\{}\\DefaultIcon", md.progid);
        let e = entries.iter().find(|e| e.key == key).unwrap();
        assert_eq!(e.data, format!("\"C:\\Program Files\\pika\\{}\",0", md.icon_file));
        assert!(!e.data.contains("\\\\"), "二重バックスラッシュが残っている: {}", e.data);
    }

    #[test]
    fn icon_dir_不明時は_defaulticon_が_exe_アプリアイコンへフォールバックする() {
        for icon_dir in [None, Some("")] {
            let entries = registration_entries(EXE, icon_dir);
            for t in ASSOC_TYPES {
                let key = format!("Software\\Classes\\{}\\DefaultIcon", t.progid);
                let e = entries.iter().find(|e| e.key == key).unwrap();
                assert_eq!(e.data, format!("\"{EXE}\",0"), "フォールバックは exe,0 のはず");
            }
        }
    }

    #[test]
    fn 登録の_command_は_exe_をダブルクオートし_percent1_を渡す() {
        let entries = registration_entries(EXE, Some(ICONS));
        let expected = format!("\"{EXE}\" \"%1\"");
        // 各 ProgId の open command と 右クリック2種の command がすべて同形であること。
        let mut command_keys: Vec<String> = ASSOC_TYPES
            .iter()
            .map(|t| format!("Software\\Classes\\{}\\shell\\open\\command", t.progid))
            .collect();
        command_keys.push("Software\\Classes\\*\\shell\\pika\\command".to_string());
        command_keys.push("Software\\Classes\\Directory\\shell\\pika\\command".to_string());
        for key in command_keys {
            let e = entries
                .iter()
                .find(|e| e.key == key && e.name.is_empty())
                .unwrap_or_else(|| panic!("command キーが無い: {key}"));
            assert_eq!(e.data, expected, "{key} の command が想定と異なる");
        }
    }

    #[test]
    fn 登録は各_progid_表示名と右クリック表示名を持つ() {
        let entries = registration_entries(EXE, Some(ICONS));
        // 各 ProgId 既定値（候補に出る表示名）。
        for t in ASSOC_TYPES {
            assert!(
                entries.iter().any(|e| e.key == format!("Software\\Classes\\{}", t.progid)
                    && e.name.is_empty()
                    && e.data == t.display),
                "{} の表示名が無い",
                t.progid
            );
        }
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
    fn 解除は各種別_progid_と旧_progid_を_deletetree_対象にする() {
        let (trees, _values) = unregistration();
        for t in ASSOC_TYPES {
            assert!(trees.contains(&format!("Software\\Classes\\{}", t.progid)));
        }
        for legacy in LEGACY_PROG_IDS {
            assert!(
                trees.contains(&format!("Software\\Classes\\{legacy}")),
                "旧 ProgId {legacy} の掃除が無い（アップグレード残骸対策）"
            );
        }
        assert!(trees.contains(&"Software\\Classes\\*\\shell\\pika".to_string()));
        assert!(trees.contains(&"Software\\Classes\\Directory\\shell\\pika".to_string()));
    }

    #[test]
    fn 解除は拡張子キーを消さず_openwithprogids_の_pika_値だけ消す() {
        let (trees, values) = unregistration();
        for t in ASSOC_TYPES {
            for ext in t.extensions {
                let owp = open_with_progids_key(ext);
                // 値削除リストに (OpenWithProgids, 対応 ProgId) があること。
                assert!(
                    values.iter().any(|(k, v)| k == &owp && v == t.progid),
                    "{ext} の OpenWithProgids から {} 値を消す指定が無い",
                    t.progid
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
        // 旧 ProgId は旧版が登録した拡張子（LEGACY_EXTENSIONS）の OpenWithProgids からも消す。
        for ext in LEGACY_EXTENSIONS {
            let owp = open_with_progids_key(ext);
            for legacy in LEGACY_PROG_IDS {
                assert!(
                    values.iter().any(|(k, v)| k == &owp && v == legacy),
                    "{ext} の OpenWithProgids から旧 {legacy} 値を消す指定が無い"
                );
            }
        }
    }

    #[test]
    fn 空キー掃除候補は各拡張子で子owpを親extより先に置く() {
        let cands = empty_delete_candidates();
        // 各拡張子につき OpenWithProgids（子）と <ext> 本体（親）の 2 つ。
        assert_eq!(cands.len(), assoc_extensions().len() * 2);
        for ext in assoc_extensions() {
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
        let entries = registration_entries(EXE, Some(ICONS));
        let (trees, _values) = unregistration();
        // DeleteTree 対象のうち、書き込む独自キー（ProgId 本体・右クリック）は登録エントリに対応すること。
        // 旧 ProgId は書き込まない（掃除専用）ので対応チェックから除外する。
        let legacy_keys: Vec<String> = LEGACY_PROG_IDS.iter().map(|p| progid_key(p)).collect();
        for t in &trees {
            if legacy_keys.contains(t) {
                continue;
            }
            let covered = entries
                .iter()
                .any(|e| e.key == *t || e.key.starts_with(&format!("{t}\\")));
            assert!(covered, "解除対象 {t} に対応する登録エントリが無い");
        }
    }
}
