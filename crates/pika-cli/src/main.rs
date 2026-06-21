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
    -h, --help       このヘルプを表示して終了する
    -v, --version    バージョンを表示して終了する
    -g <spec>        指定位置にカーソルを置いて開く（VS Code 互換）

注記:
    本 console CLI は引数を検証し絶対パス正規化して GUI（pika.exe）を起動する。
    既に起動済みのときは GUI 側が単一インスタンスとして引数を転送し前面化する。
";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    run(&args)
}

/// 引数を解釈して終了コードを返す。標準出力/標準エラーへの出力もここで行う。
///
/// パース規則は `pika-core::cli` に集約し、ここでは「制御フラグ処理・絶対パス正規化・GUI 起動」を行う。
fn run(args: &[String]) -> ExitCode {
    // 先頭の制御フラグを処理（--help / --version は他引数より優先・同期完結）。
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
