//! pika console subsystem CLI（design doc 9章 二段構成の前段）。
//!
//! `pika-cli.exe`（console subsystem）が `--help`/`--version`/引数検証/終了コードを
//! **同期処理**し、GUI 起動が必要なときのみ `pika.exe`（windows subsystem）を spawn する。
//! 本スプリント（sprint 1）では二段構成の console 側スタブとして
//! `--help`/`--version`/`-g` パース/終了コードを成立させる（GUI spawn の実配線は sprint 5）。
//!
//! 終了コード規約（要件3.1/3.4）:
//! - 0  = 受理（引数を解釈できた。ファイルが開かれた保証ではない）
//! - 2  = 引数エラー（不正な引数）

#![cfg_attr(windows, windows_subsystem = "console")]

use std::process::ExitCode;

const VERSION: &str = env!("CARGO_PKG_VERSION");

const HELP: &str = "\
pika — Windows 向け超軽量 Markdown/HTML エディタ

使い方:
    pika [オプション] [パス]
    pika -g <ファイル>[:<行>[:<桁>]]

オプション:
    -h, --help       このヘルプを表示して終了する
    -v, --version    バージョンを表示して終了する
    -g <spec>        指定位置にカーソルを置いて開く（VS Code 互換）

注記:
    GUI の起動・単一インスタンス転送は sprint 5 で実装する。
    本スタブは console 側で引数を検証し終了コードを返すところまでを担う。
";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    run(&args)
}

/// 引数を解釈して終了コードを返す。標準出力/標準エラーへの出力もここで行う。
fn run(args: &[String]) -> ExitCode {
    // 先頭の制御フラグを処理（--help / --version は他引数より優先）。
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

    // `-g <spec>` の検証（パース規則は pika-core::cli に集約）。
    if let Some(pos) = args.iter().position(|a| a == "-g") {
        match args.get(pos + 1) {
            Some(spec) => match pika_core::cli::parse_goto_spec(spec) {
                Ok(target) => {
                    // sprint 1 ではパース成立を確認するのみ（GUI 起動は sprint 5）。
                    println!("goto: {}", target.file);
                    return ExitCode::SUCCESS;
                }
                Err(e) => {
                    eprintln!("エラー: {e}");
                    return ExitCode::from(2);
                }
            },
            None => {
                eprintln!("エラー: -g にはファイル指定が必要です");
                return ExitCode::from(2);
            }
        }
    }

    // 引数なし、または単なるパス列はとりあえず受理（GUI 配線は後続スプリント）。
    ExitCode::SUCCESS
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
        // -g の後続が無い場合は非 0 で返す（引数エラー）。
        assert_eq!(
            format!("{:?}", run(&["-g".into()])),
            format!("{:?}", ExitCode::from(2))
        );
    }

    #[test]
    fn g_の正常指定は成功で返る() {
        assert_eq!(
            format!("{:?}", run(&["-g".into(), r"C:\a.md:3:2".into()])),
            format!("{:?}", ExitCode::SUCCESS)
        );
    }
}
