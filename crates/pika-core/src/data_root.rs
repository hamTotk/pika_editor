//! データルートの解決（要件13・design doc 9章）。
//!
//! 起動最初期に 1 回だけ解決し、全モジュールへ確定パスを渡す方針（design doc 9章）。
//! 解決規則を I/O 非依存の純粋関数に切り出して cargo test で固める
//! （実 FS 探索/環境変数取得は呼び出し側＝`pika-cli`/`src-tauri` が行い、判定だけここに集約）。
//!
//! 規則（design doc 9章）:
//! - exe 隣に `portable.txt` があれば `<exe_dir>/pika-data/`（ポータブル版）。
//! - 無ければ `%LOCALAPPDATA%\pika\`（インストール版）。
//! - ワークスペースは汚さない（ユーザーフォルダに pika のファイルを置かない）。

use crate::error::{PikaError, Result};
use std::path::PathBuf;

/// データルート解決の分岐種別（観測可能にしてテストで分岐を検証する）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DataRootKind {
    /// exe 隣に portable.txt があった（`<exe_dir>/pika-data/`）。
    Portable,
    /// portable.txt が無く `%LOCALAPPDATA%\pika\` を使う。
    Installed,
}

/// データルートの解決結果。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataRoot {
    /// データルートの絶対パス。
    pub path: PathBuf,
    /// どちらの分岐で解決したか。
    pub kind: DataRootKind,
}

/// データルートを解決する純粋関数。
///
/// - `exe_dir`: 実行ファイルの所在ディレクトリ。
/// - `portable_txt_present`: `exe_dir/portable.txt` が存在するか（呼び出し側が FS で判定）。
/// - `local_appdata`: `%LOCALAPPDATA%` の値（インストール版で使用。ポータブル版では参照しない）。
///
/// ポータブル分岐では `local_appdata` 不要なので、欠落していてもエラーにしない（軽い・固まらない）。
pub fn resolve_data_root(
    exe_dir: &str,
    portable_txt_present: bool,
    local_appdata: Option<&str>,
) -> Result<DataRoot> {
    if portable_txt_present {
        let mut path = PathBuf::from(exe_dir);
        path.push("pika-data");
        return Ok(DataRoot {
            path,
            kind: DataRootKind::Portable,
        });
    }

    let base = local_appdata.ok_or_else(|| {
        PikaError::PathResolution("%LOCALAPPDATA% が未設定でデータルートを解決できない".into())
    })?;
    if base.is_empty() {
        return Err(PikaError::PathResolution(
            "%LOCALAPPDATA% が空でデータルートを解決できない".into(),
        ));
    }
    let mut path = PathBuf::from(base);
    path.push("pika");
    Ok(DataRoot {
        path,
        kind: DataRootKind::Installed,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn portable_txt_ありはポータブル分岐() {
        let r =
            resolve_data_root(r"C:\Apps\pika", true, Some(r"C:\Users\u\AppData\Local")).unwrap();
        assert_eq!(r.kind, DataRootKind::Portable);
        assert_eq!(r.path, PathBuf::from(r"C:\Apps\pika\pika-data"));
    }

    #[test]
    fn portable_txt_なしはインストール分岐() {
        let r = resolve_data_root(
            r"C:\Program Files\pika",
            false,
            Some(r"C:\Users\u\AppData\Local"),
        )
        .unwrap();
        assert_eq!(r.kind, DataRootKind::Installed);
        assert_eq!(r.path, PathBuf::from(r"C:\Users\u\AppData\Local\pika"));
    }

    #[test]
    fn ポータブルは_localappdata_不在でも解決できる() {
        // ポータブル分岐は %LOCALAPPDATA% を参照しないため None でも成功する。
        let r = resolve_data_root(r"D:\portable\pika", true, None).unwrap();
        assert_eq!(r.kind, DataRootKind::Portable);
        assert_eq!(r.path, PathBuf::from(r"D:\portable\pika\pika-data"));
    }

    #[test]
    fn インストール分岐で_localappdata_不在はエラー() {
        assert!(resolve_data_root(r"C:\Program Files\pika", false, None).is_err());
    }

    #[test]
    fn インストール分岐で_localappdata_空文字はエラー() {
        assert!(resolve_data_root(r"C:\Program Files\pika", false, Some("")).is_err());
    }
}
