//! pika console subsystem CLI（design doc 9章 二段構成の前段）。
//!
//! `pika-cli.exe`（console subsystem）が `--help`/`--version`/引数検証/終了コードを
//! **同期処理**し、GUI 起動が必要なときのみ `pika.exe`（windows subsystem）を spawn する。
//! 単一インスタンスの判定（named pipe 獲得/転送）は GUI 側（`pika.exe`）が原子的に行うため、
//! CLI は「引数を検証し、絶対パス正規化して GUI を起動する」前段に徹する（要件3.1/3.2）。
//!
//! 終了コード規約（要件3.1/3.4）:
//! - 0  = 受理（引数を解釈できた／GUI へ転送できた。ファイルが開かれた保証ではない）
//! - 2  = 引数エラー（不正な引数）
//! - 3  = GUI 起動の失敗（pika.exe を spawn できない等）

#![cfg_attr(windows, windows_subsystem = "console")]

use std::process::ExitCode;

const VERSION: &str = env!("CARGO_PKG_VERSION");

const HELP: &str = "\
pika — Windows 向け超軽量 Markdown/HTML エディタ

使い方:
    pika [オプション] [パス...]
    pika -g <ファイル>[:<行>[:<桁>]]

オプション:
    -h, --help            このヘルプを表示して終了する
    -v, --version         バージョンを表示して終了する
    -g <spec>             指定位置にカーソルを置いて開く（VS Code 互換）
    --register-shell      エクスプローラー統合を登録する（HKCU・「pikaで開く」/関連付け候補）
    --unregister-shell    エクスプローラー統合を解除する

注記:
    本 console CLI は引数を検証し絶対パス正規化して GUI（pika.exe）を起動する。
    既に起動済みのときは GUI 側が単一インスタンスとして引数を転送し前面化する。
    --register-shell は HKCU\\Software\\Classes に候補（OpenWithProgids）と右クリック
    「pikaで開く」を追加する（管理者不要）。Windows は既定アプリを強制設定できないため、
    最後にエクスプローラーで対象を右クリック→「プログラムから開く」→「常にこのアプリ」を選ぶ。
";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    run(&args)
}

/// 引数を解釈して終了コードを返す。標準出力/標準エラーへの出力もここで行う。
///
/// パース規則は `pika-core::cli` に集約し、ここでは「制御フラグ処理・絶対パス正規化・GUI 起動」を行う。
fn run(args: &[String]) -> ExitCode {
    // 先頭の制御フラグを処理（--help / --version / --register-shell / --unregister-shell は
    // 他引数より優先・同期完結。GUI 起動には進まない）。
    for a in args {
        match a.as_str() {
            "-h" | "--help" => {
                print!("{HELP}");
                return ExitCode::SUCCESS;
            }
            "-v" | "--version" => {
                // リダイレクト取得（pika --version > v.txt）でも文字化けしないよう素の文字列のみ。
                println!("pika {VERSION}");
                return ExitCode::SUCCESS;
            }
            "--register-shell" => return register_shell(),
            "--unregister-shell" => return unregister_shell(),
            _ => {}
        }
    }

    // GUI へ渡す引数を組み立てる（絶対パス正規化済み）。
    let forward = match build_forward_args(args) {
        Ok(f) => f,
        Err(code) => return code,
    };

    // GUI（pika.exe）を起動する。単一インスタンス判定は GUI 側が named pipe で原子的に行う。
    launch_gui(&forward)
}

/// 検証/絶対パス正規化を経て GUI へ渡す引数列を組み立てる。エラー時は終了コードを返す。
///
/// `-g <spec>` はファイルとカーソル位置に分解し、`<file>` を絶対パス化して `-g <abs>:<行>[:<桁>]` へ
/// 組み直す（要件3.1「相対パスはクライアント側で絶対パス正規化」）。それ以外のパス引数も絶対化する。
fn build_forward_args(args: &[String]) -> Result<Vec<String>, ExitCode> {
    let cwd = std::env::current_dir()
        .map(|p| p.to_string_lossy().to_string())
        .unwrap_or_default();
    let mut out: Vec<String> = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "-g" {
            let spec = match args.get(i + 1) {
                Some(s) => s,
                None => {
                    eprintln!("エラー: -g にはファイル指定が必要です");
                    return Err(ExitCode::from(2));
                }
            };
            let target = match pika_core::cli::parse_goto_spec(spec) {
                Ok(t) => t,
                Err(e) => {
                    eprintln!("エラー: {e}");
                    return Err(ExitCode::from(2));
                }
            };
            let abs = match pika_core::cli::normalize_to_absolute(&target.file, &cwd) {
                Ok(p) => p,
                Err(e) => {
                    eprintln!("エラー: {e}");
                    return Err(ExitCode::from(2));
                }
            };
            out.push("-g".into());
            out.push(rebuild_goto_spec(&abs, target.line, target.column));
            i += 2;
            continue;
        }
        // 通常のパス引数（オプションでない）は絶対パス化する。
        if a.starts_with('-') {
            // 未知オプションは GUI へそのまま渡さず弾く（受理操作=パスオープン限定の前段防御）。
            eprintln!("エラー: 不明なオプション: {a}");
            return Err(ExitCode::from(2));
        }
        match pika_core::cli::normalize_to_absolute(a, &cwd) {
            Ok(p) => out.push(p),
            Err(e) => {
                eprintln!("エラー: {e}");
                return Err(ExitCode::from(2));
            }
        }
        i += 1;
    }
    Ok(out)
}

/// `-g` の絶対パス＋行/桁を `<abs>:<行>[:<桁>]` 文字列へ組み直す。
fn rebuild_goto_spec(abs: &str, line: Option<u32>, column: Option<u32>) -> String {
    match (line, column) {
        (Some(l), Some(c)) => format!("{abs}:{l}:{c}"),
        (Some(l), None) => format!("{abs}:{l}"),
        _ => abs.to_string(),
    }
}

/// GUI（pika.exe）を spawn する。CLI は GUI の終了を待たない（要件14章「`--wait` はやらない」）。
///
/// pika.exe は同じディレクトリ（exe 隣）に同梱される想定（bundler 配置・design doc 9章）。
/// spawn 成功で受理（終了コード0）。GUI 側が単一インスタンス転送を担う。
fn launch_gui(forward: &[String]) -> ExitCode {
    let gui = match gui_exe_path() {
        Some(p) => p,
        None => {
            eprintln!("エラー: GUI（pika.exe）の場所を特定できません");
            return ExitCode::from(3);
        }
    };
    match std::process::Command::new(&gui).args(forward).spawn() {
        Ok(_) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("エラー: GUI を起動できません（{}）: {e}", gui.display());
            ExitCode::from(3)
        }
    }
}

/// pika.exe の絶対パスを解決する（pika-cli.exe と同じディレクトリの隣に置かれる）。
fn gui_exe_path() -> Option<std::path::PathBuf> {
    let mut dir = std::env::current_exe().ok()?;
    dir.pop();
    dir.push(if cfg!(windows) { "pika.exe" } else { "pika" });
    Some(dir)
}

// ── エクスプローラー統合（要件3.3 `--register-shell` / `--unregister-shell`）─────────────
//
// 書き込む/消すキー・値の決定は純粋ロジック pika_core::explorer（cargo test 済み）に集約し、
// ここは HKCU\Software\Classes への実レジストリ書込/削除（Reg*）と関連付け変更通知
// （SHChangeNotify）という OS 呼び出しの薄いラッパに徹する（design doc 3章）。

/// 終了コード: レジストリ書込/削除に失敗（要件3.4 の終了コード規約に 4 を追加）。
const EXIT_REGISTRY_FAILURE: u8 = 4;

/// エクスプローラー統合を登録する（HKCU・候補登録＋右クリック「pikaで開く」）。
#[cfg(windows)]
fn register_shell() -> ExitCode {
    // 関連付け先の実行体は GUI（pika.exe）。コンソール窓を出さずに開くため pika-cli.exe ではない。
    let exe = match gui_exe_path() {
        Some(p) => p.to_string_lossy().to_string(),
        None => {
            eprintln!("エラー: GUI（pika.exe）の場所を特定できません");
            return ExitCode::from(3);
        }
    };
    let entries = pika_core::explorer::registration_entries(&exe);
    for e in &entries {
        if let Err(msg) = win::write_reg_sz(&e.key, &e.name, &e.data) {
            eprintln!("エラー: レジストリ書込に失敗しました（{}）: {msg}", e.key);
            return ExitCode::from(EXIT_REGISTRY_FAILURE);
        }
    }
    // エクスプローラーへ関連付け変更を即時反映させる。
    win::notify_assoc_changed();
    println!("エクスプローラー統合を登録しました（pika.exe = {exe}）。");
    println!(
        "Windows は既定アプリを強制設定できないため、最後にユーザー操作が必要です:\n  \
         エクスプローラーで対象ファイル（.md/.markdown/.html/.htm）を右クリック →\n  \
         「プログラムから開く」→「別のプログラムを選択」→ pika を選び「常にこのアプリで開く」。\n  \
         フォルダ/ファイルの右クリックには「pikaで開く」が追加されています。"
    );
    ExitCode::SUCCESS
}

/// エクスプローラー統合を解除する（独自キーの DeleteTree ＋ OpenWithProgids の pika 値削除）。
#[cfg(windows)]
fn unregister_shell() -> ExitCode {
    let (trees, values) = pika_core::explorer::unregistration();
    for key in &trees {
        if let Err(msg) = win::delete_tree(key) {
            eprintln!("エラー: レジストリキーの削除に失敗しました（{key}）: {msg}");
            return ExitCode::from(EXIT_REGISTRY_FAILURE);
        }
    }
    for (key, name) in &values {
        if let Err(msg) = win::delete_value(key, name) {
            eprintln!("エラー: レジストリ値の削除に失敗しました（{key}\\{name}）: {msg}");
            return ExitCode::from(EXIT_REGISTRY_FAILURE);
        }
    }
    win::notify_assoc_changed();
    println!("エクスプローラー統合を解除しました（候補・右クリックを削除）。");
    ExitCode::SUCCESS
}

/// 非 Windows ではエクスプローラー統合を持たない（pika は Windows 専用＝通常到達しない）。
#[cfg(not(windows))]
fn register_shell() -> ExitCode {
    eprintln!("エラー: エクスプローラー統合は Windows でのみ対応しています");
    ExitCode::from(EXIT_REGISTRY_FAILURE)
}

/// 非 Windows ではエクスプローラー統合を持たない（pika は Windows 専用＝通常到達しない）。
#[cfg(not(windows))]
fn unregister_shell() -> ExitCode {
    eprintln!("エラー: エクスプローラー統合は Windows でのみ対応しています");
    ExitCode::from(EXIT_REGISTRY_FAILURE)
}

/// HKCU への実レジストリ書込/削除・関連付け変更通知（OS 呼び出しの薄いラッパ）。
#[cfg(windows)]
mod win {
    use std::os::windows::ffi::OsStrExt;
    use std::ptr;
    use windows_sys::Win32::Foundation::{ERROR_FILE_NOT_FOUND, ERROR_SUCCESS};
    use windows_sys::Win32::System::Registry::{
        RegCloseKey, RegCreateKeyExW, RegDeleteKeyValueW, RegDeleteTreeW, RegSetValueExW, HKEY,
        HKEY_CURRENT_USER, KEY_WRITE, REG_OPTION_NON_VOLATILE, REG_SZ,
    };
    use windows_sys::Win32::UI::Shell::{SHChangeNotify, SHCNE_ASSOCCHANGED, SHCNF_IDLIST};

    /// UTF-8 → NUL 終端 UTF-16（Win32 境界変換）。
    fn to_wide(s: &str) -> Vec<u16> {
        std::ffi::OsStr::new(s)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    }

    /// `HKCU\<key>` の値 `name`（""=既定値）へ `REG_SZ` の `data` を書く（キーが無ければ作る）。
    pub fn write_reg_sz(key: &str, name: &str, data: &str) -> Result<(), String> {
        let key_w = to_wide(key);
        let mut hkey: HKEY = ptr::null_mut();
        // SAFETY: key_w は NUL 終端。lpClass/lpSecurityAttributes は不要なので null。phkresult のみ受ける。
        let rc = unsafe {
            RegCreateKeyExW(
                HKEY_CURRENT_USER,
                key_w.as_ptr(),
                0,
                ptr::null(),
                REG_OPTION_NON_VOLATILE,
                KEY_WRITE,
                ptr::null(),
                &mut hkey,
                ptr::null_mut(),
            )
        };
        if rc != ERROR_SUCCESS {
            return Err(format!("キー作成に失敗（コード {rc}）"));
        }
        let name_w = to_wide(name);
        // REG_SZ は NUL 終端付き UTF-16。バイト長（NUL 含む）を渡す。
        let data_w = to_wide(data);
        let data_bytes = data_w.len() * std::mem::size_of::<u16>();
        // SAFETY: hkey は有効。name_w は NUL 終端。data_w は data_bytes バイトの有効な UTF-16。
        let set = unsafe {
            RegSetValueExW(
                hkey,
                name_w.as_ptr(),
                0,
                REG_SZ,
                data_w.as_ptr() as *const u8,
                data_bytes as u32,
            )
        };
        // SAFETY: hkey は RegCreateKeyExW が返した有効なハンドル。
        unsafe {
            RegCloseKey(hkey);
        }
        if set != ERROR_SUCCESS {
            return Err(format!("値設定に失敗（コード {set}）"));
        }
        Ok(())
    }

    /// `HKCU\<key>` をサブキーごと削除する（既に無い場合も成功扱い＝冪等）。
    pub fn delete_tree(key: &str) -> Result<(), String> {
        let key_w = to_wide(key);
        // SAFETY: key_w は NUL 終端。HKCU 配下のみを対象にする（root を消さない）。
        let rc = unsafe { RegDeleteTreeW(HKEY_CURRENT_USER, key_w.as_ptr()) };
        if rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND {
            Ok(())
        } else {
            Err(format!("キー削除に失敗（コード {rc}）"))
        }
    }

    /// `HKCU\<key>` の値 `name` だけを削除する（キー本体は残す。既に無い場合も成功扱い）。
    pub fn delete_value(key: &str, name: &str) -> Result<(), String> {
        let key_w = to_wide(key);
        let name_w = to_wide(name);
        // SAFETY: key_w / name_w は NUL 終端。キーを開いて値を消し閉じるまでを 1 呼び出しで行う。
        let rc = unsafe { RegDeleteKeyValueW(HKEY_CURRENT_USER, key_w.as_ptr(), name_w.as_ptr()) };
        if rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND {
            Ok(())
        } else {
            Err(format!("値削除に失敗（コード {rc}）"))
        }
    }

    /// 関連付け変更をエクスプローラーへ通知する（候補/右クリックを即時反映）。
    pub fn notify_assoc_changed() {
        // SAFETY: 引数なしの通知（dwItem は null）。副作用は OS への変更通知のみ。
        unsafe {
            SHChangeNotify(
                SHCNE_ASSOCCHANGED as i32,
                SHCNF_IDLIST as u32,
                ptr::null(),
                ptr::null(),
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn help_は成功で返る() {
        assert_eq!(
            format!("{:?}", run(&["--help".into()])),
            format!("{:?}", ExitCode::SUCCESS)
        );
    }

    #[test]
    fn version_は成功で返る() {
        assert_eq!(
            format!("{:?}", run(&["--version".into()])),
            format!("{:?}", ExitCode::SUCCESS)
        );
    }

    #[test]
    fn g_に引数なしはエラーコード() {
        // -g の後続が無い場合は非 0（引数エラー）。build_forward_args が弾く。
        assert!(build_forward_args(&["-g".into()]).is_err());
    }

    #[test]
    fn g_の不正な_spec_は引数エラー() {
        // 空 spec は parse_goto_spec がエラー。
        assert!(build_forward_args(&["-g".into(), "".into()]).is_err());
    }

    #[test]
    fn 未知オプションは引数エラー() {
        assert!(build_forward_args(&["--bogus".into()]).is_err());
    }

    #[test]
    fn g_の絶対パス指定は転送引数を組み立てる() {
        let f = build_forward_args(&["-g".into(), r"C:\a.md:3:2".into()]).unwrap();
        assert_eq!(f, vec!["-g".to_string(), r"C:\a.md:3:2".to_string()]);
    }

    #[test]
    fn g_行のみ指定は桁を付けずに組み立てる() {
        let f = build_forward_args(&["-g".into(), r"C:\a.md:7".into()]).unwrap();
        assert_eq!(f, vec!["-g".to_string(), r"C:\a.md:7".to_string()]);
    }

    #[test]
    fn 絶対パスの通常引数はそのまま転送する() {
        let f = build_forward_args(&[r"C:\dir\note.md".into()]).unwrap();
        assert_eq!(f, vec![r"C:\dir\note.md".to_string()]);
    }

    #[test]
    fn goto_spec_組み直し_桁あり() {
        assert_eq!(
            rebuild_goto_spec(r"C:\a.md", Some(12), Some(3)),
            r"C:\a.md:12:3"
        );
    }

    #[test]
    fn goto_spec_組み直し_桁なし() {
        assert_eq!(rebuild_goto_spec(r"C:\a.md", Some(12), None), r"C:\a.md:12");
    }

    #[test]
    fn goto_spec_組み直し_位置なし() {
        assert_eq!(rebuild_goto_spec(r"C:\a.md", None, None), r"C:\a.md");
    }
}
