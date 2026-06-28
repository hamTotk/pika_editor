//! 非テキストファイルとエッジケースの縮退判定（要件12.1/12.2・design doc 8章/19章）。
//!
//! 本モジュールは UI/Tauri/wry/FS を一切知らない**純粋ロジック**（cargo test の決定論ゲート対象）。
//! 「このファイルを画像簡易ビューで開くか／巨大画像でデコードを避けるか／非対応バイナリとして
//! 『既定アプリで開く』へ誘導するか／FS エッジ（読み取り専用・ネットワーク・クラウド）でどう縮退して
//! アプリを継続するか」の**意思決定**だけを担い、実デコード・実 FS 操作は呼び出し側が結果に従って行う。
//!
//! 設計原則「固まらない」: 巨大画像のデコード爆発を**デコード前の寸法プリチェック**で防ぐ（要件12.2）。

/// 画像の総ピクセル数上限（要件2.2 のレンダリングガード＝6000万px）。
///
/// ヘッダから得た `width*height` がこれを**超える**とデコードせず「既定アプリで開く」へ誘導する
/// （巨大画像のデコード爆発で固まらないため＝要件12.2）。値の単一 source は
/// [`crate::render::guard::DEFAULT_IMAGE_MAX_PIXELS`]（同値を参照し二重定義のドリフトを断つ＝eval low）。
pub const MAX_IMAGE_PIXELS: u64 = crate::render::guard::DEFAULT_IMAGE_MAX_PIXELS;

/// ファイルの開き方の分類（要件12.2）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FileKind {
    /// テキストとして開く（エディタ/差分/プレビューの対象）。
    Text,
    /// 画像簡易ビューで開く（表示のみ・編集/変換なし＝要件12.2）。
    Image,
    /// 非対応バイナリ。「対応していない形式」表示＋「既定アプリで開く」（要件12.2）。
    UnsupportedBinary,
}

/// 拡張子（小文字・ドットなし）からファイル種別を分類する（要件12.2）。
///
/// 画像拡張子（要件12.2 の列挙）は画像簡易ビュー、既知テキスト拡張子はテキスト、
/// それ以外の既知バイナリ/不明拡張子は非対応バイナリへ倒す（安全側＝固まらない）。
/// 拡張子だけで内容を信頼しきらず、最終的な「画像として描画してよいか」は寸法プリチェック
/// （[`decide_image_open`]）で再判定する。
pub fn classify_extension(ext_lowercase: &str) -> FileKind {
    const IMAGE_EXTS: &[&str] = &["png", "jpg", "jpeg", "gif", "webp", "bmp", "ico"];
    const TEXT_EXTS: &[&str] = &[
        "md", "markdown", "html", "htm", "txt", "json", "jsonl", "csv", "tsv", "xml", "yaml",
        "yml", "toml", "rs", "ts", "js", "tsx", "jsx", "css", "py", "c", "cpp", "h", "hpp", "go",
        "java", "sh", "log",
    ];
    let e = ext_lowercase.trim_start_matches('.');
    if IMAGE_EXTS.contains(&e) {
        FileKind::Image
    } else if TEXT_EXTS.contains(&e) {
        FileKind::Text
    } else {
        // 未知拡張子は安全側で非対応バイナリ扱い（誤って巨大バイナリをテキスト全量ロードしない）。
        FileKind::UnsupportedBinary
    }
}

/// **ファイル名**からファイル種別を分類する（要件12.2・拡張子なし/dotfile をテキストへ寄せる）。
///
/// [`classify_extension`] は拡張子のみを見るため、拡張子を持たない `Dockerfile`/`Makefile`/`LICENSE`/
/// `README` や、先頭ドットの dotfile `.gitignore`/`.editorconfig`/`.env` を「不明拡張子」として
/// 非対応バイナリへ倒し、テキストなのに CM6 で開けず「既定アプリで開く」しか出せなくなる（回帰）。
/// 本関数は**ファイル名全体**を見て、これらを安全にテキストへ寄せる:
/// 1. 名前にドットを含まない（拡張子なし＝`Dockerfile`/`Makefile`/`README`/`LICENSE`）→ [`FileKind::Text`]。
/// 2. dotfile（先頭が `.` で、2文字目以降にドットを含まない＝`.gitignore`/`.editorconfig`/`.env`）
///    → [`FileKind::Text`]。
/// 3. それ以外は**最後のドット以降**を小文字化した拡張子を [`classify_extension`] へ委譲（既存挙動を維持）。
///
/// 取りこぼし（許容）: `.env.local` のような「先頭ドット＋途中にもドット」は dotfile 規則に当たらず、
/// 最後のドット以降（`local`）が未知拡張子なので [`FileKind::UnsupportedBinary`] になる。
/// 機密ファイル（`.env` 等）がテキスト分類になっても、ベースラインは HashOnly（差分非対象）・custom
/// protocol 配信拒否のまま安全（種別はエディタ表示可否のみで、機密の取り扱いは別系統が担保する）。
pub fn classify_file_name(name: &str) -> FileKind {
    // 1. 拡張子なし（ドットを一切含まない）はテキストへ寄せる（Dockerfile/Makefile/README/LICENSE）。
    if !name.contains('.') {
        return FileKind::Text;
    }
    // 2. dotfile（先頭 `.` で、それ以降にドットが無い）はテキストへ寄せる（.gitignore/.editorconfig/.env）。
    if let Some(rest) = name.strip_prefix('.') {
        if !rest.contains('.') {
            return FileKind::Text;
        }
    }
    // 3. それ以外は最後のドット以降の拡張子（小文字化）で従来判定（classify_extension に委譲）。
    let ext = name.rsplit('.').next().unwrap_or("").to_lowercase();
    classify_extension(&ext)
}

/// 画像を開く際の判定（要件12.2 寸法プリチェック）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ImageOpenDecision {
    /// デコードして簡易ビューで表示してよい（寸法が上限内）。
    Decode {
        /// 総ピクセル数（呼び出し側のメモリ見積り用）。
        pixels: u64,
    },
    /// 寸法が上限超。デコードせず「既定アプリで開く」へ誘導する（固まらない＝要件12.2）。
    OpenExternally {
        /// 通知文言用の総ピクセル数。
        pixels: u64,
    },
    /// ヘッダから寸法が取れなかった（壊れ画像等）。デコードせず外部誘導（安全側）。
    DimensionsUnknown,
}

/// ヘッダから得た寸法でデコード可否を判定する（要件12.2）。
///
/// - `dims`: ヘッダから取得した `(width, height)`。取得できなければ `None`。
/// - 総ピクセル数が [`MAX_IMAGE_PIXELS`] を**超える**ならデコードせず外部誘導（デコード爆発回避）。
/// - **寸法不明時の既定（#24・fail-closed で統一）**: `None`（壊れ/細工/未知形式）は
///   [`ImageOpenDecision::DimensionsUnknown`]＝デコードせず「既定アプリで開く」へ誘導する（安全側）。
///   配信前ガード [`crate::render::guard::check_image_bytes`] も寸法不明を Block（外部誘導）に倒し、
///   本関数と方針を揃えている（以前は guard=Allow（危険側）で割れていた）。
pub fn decide_image_open(dims: Option<(u32, u32)>) -> ImageOpenDecision {
    match dims {
        None => ImageOpenDecision::DimensionsUnknown,
        Some((w, h)) => {
            let pixels = w as u64 * h as u64;
            if pixels > MAX_IMAGE_PIXELS {
                ImageOpenDecision::OpenExternally { pixels }
            } else {
                ImageOpenDecision::Decode { pixels }
            }
        }
    }
}

/// FS エッジの縮退理由（要件12.1・機能を縮退してアプリ継続＋次の一手提示）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FsEdge {
    /// 読み取り専用属性（開けるが保存時に「名前を付けて保存」/属性解除誘導＝要件12.1）。
    ReadOnly,
    /// アクセス権なし・排他ロック（リトライ後エラー表示・クラッシュしない＝要件12.1）。
    AccessDenied,
    /// ネットワークドライブ/UNC（監視不可なら定期ポーリングへフォールバック＝要件12.1）。
    NetworkDrive,
    /// OneDrive 等クラウドプレースホルダ（ベースライン取得対象から除外・開いたときのみ読む＝要件12.1）。
    CloudPlaceholder,
    /// 開いているフォルダ自体の削除/ドライブ切断（空状態へ安全遷移＝要件12.1）。
    WorkspaceGone,
}

/// FS エッジでの縮退方針（呼び出し側が UI 縮退＋次の一手提示に使う）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DegradePlan {
    /// 編集を許すか（読み取り専用は保存導線で誘導するので開いて編集は許す）。
    pub allow_edit: bool,
    /// ベースライン内容取得の対象にするか（クラウドプレースホルダは除外＝要件12.1/9章）。
    pub take_baseline: bool,
    /// 監視をポーリングへフォールバックするか（ネットワークドライブ＝要件12.1）。
    pub poll_fallback: bool,
    /// ワークスペースを空状態へ安全遷移させるか（フォルダ削除/ドライブ切断＝要件12.1）。
    pub empty_workspace: bool,
}

/// FS エッジから縮退方針を決める（要件12.1「機能を縮退してアプリ継続＋次の一手提示」）。
///
/// どのエッジでもクラッシュ/フリーズさせず、可能な機能だけ残してアプリを継続する（最上位原則寄り）。
pub fn degrade_for_edge(edge: FsEdge) -> DegradePlan {
    match edge {
        // 読み取り専用: 開けて編集もできる（保存時に誘導）。ベースラインは取れる。
        FsEdge::ReadOnly => DegradePlan {
            allow_edit: true,
            take_baseline: true,
            poll_fallback: false,
            empty_workspace: false,
        },
        // アクセス権なし: 編集もベースラインも不可。リトライ後エラー表示（クラッシュしない）。
        FsEdge::AccessDenied => DegradePlan {
            allow_edit: false,
            take_baseline: false,
            poll_fallback: false,
            empty_workspace: false,
        },
        // ネットワークドライブ: 開けるが監視はポーリングへ。ベースラインは取れる。
        FsEdge::NetworkDrive => DegradePlan {
            allow_edit: true,
            take_baseline: true,
            poll_fallback: true,
            empty_workspace: false,
        },
        // クラウドプレースホルダ: フォルダを開いただけで全 DL しないようベースライン取得から除外。
        FsEdge::CloudPlaceholder => DegradePlan {
            allow_edit: true,
            take_baseline: false,
            poll_fallback: false,
            empty_workspace: false,
        },
        // ワークスペース消失: 空状態へ安全遷移（要件12.1・要件10.1 の3分岐の1つ）。
        FsEdge::WorkspaceGone => DegradePlan {
            allow_edit: false,
            take_baseline: false,
            poll_fallback: false,
            empty_workspace: true,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 画像拡張子は画像簡易ビューへ分類する() {
        for e in ["png", "jpg", "jpeg", "gif", "webp", "bmp", "ico"] {
            assert_eq!(classify_extension(e), FileKind::Image, "ext={e}");
        }
        // ドット付きでも正規化される。
        assert_eq!(
            classify_extension(".PNG".to_lowercase().as_str()),
            FileKind::Image
        );
    }

    #[test]
    fn 既知テキスト拡張子はテキストへ分類する() {
        for e in ["md", "html", "txt", "json", "rs", "ts"] {
            assert_eq!(classify_extension(e), FileKind::Text, "ext={e}");
        }
    }

    #[test]
    fn 未知拡張子は非対応バイナリへ安全側で倒す() {
        // 未知/既知バイナリはテキスト全量ロードを避け非対応バイナリ扱い（要件12.2）。
        assert_eq!(classify_extension("exe"), FileKind::UnsupportedBinary);
        assert_eq!(classify_extension("zip"), FileKind::UnsupportedBinary);
        assert_eq!(
            classify_extension("unknownext"),
            FileKind::UnsupportedBinary
        );
    }

    #[test]
    fn ファイル名分類_拡張子なしとdotfileはテキストへ寄せる() {
        // 拡張子なし（ドット無し）はテキスト（CM6 で開ける・回帰修正）。
        for n in ["Dockerfile", "Makefile", "README", "LICENSE"] {
            assert_eq!(classify_file_name(n), FileKind::Text, "name={n}");
        }
        // dotfile（先頭ドット＋以降ドット無し）はテキスト。
        for n in [".gitignore", ".editorconfig", ".env"] {
            assert_eq!(classify_file_name(n), FileKind::Text, "name={n}");
        }
    }

    #[test]
    fn ファイル名分類_拡張子ありは従来どおりclassify_extensionへ委譲する() {
        // 画像拡張子（大文字も小文字化して判定）。
        assert_eq!(classify_file_name("a.png"), FileKind::Image);
        assert_eq!(classify_file_name("photo.JPG"), FileKind::Image);
        // 既知テキスト拡張子。
        assert_eq!(classify_file_name("notes.md"), FileKind::Text);
        // 既知/未知バイナリは非対応バイナリへ倒す（安全側）。
        assert_eq!(classify_file_name("a.exe"), FileKind::UnsupportedBinary);
        assert_eq!(classify_file_name("a.zip"), FileKind::UnsupportedBinary);
        // 取りこぼし（許容・コメント明記）: `.env.local` は dotfile 規則に当たらず、最後のドット
        // 以降 `local` が未知拡張子なので UnsupportedBinary（dotfile としては取りこぼす）。
        assert_eq!(classify_file_name(".env.local"), FileKind::UnsupportedBinary);
    }

    #[test]
    fn 寸法上限内ならデコードする() {
        // 6000万px ちょうどは上限内（「超える」でのみ外部誘導）。
        let d = decide_image_open(Some((6000, 10000))); // = 6000万px ちょうど。
        assert_eq!(
            d,
            ImageOpenDecision::Decode {
                pixels: MAX_IMAGE_PIXELS
            }
        );
    }

    #[test]
    fn 寸法上限超はデコードせず外部誘導する() {
        // 要件12.2: 6000万px 超はデコード爆発回避でデコードしない。
        let d = decide_image_open(Some((10000, 10000))); // = 1億px。
        assert_eq!(
            d,
            ImageOpenDecision::OpenExternally {
                pixels: 100_000_000
            }
        );
    }

    #[test]
    fn 寸法不明はデコードせず安全側で外部誘導() {
        // 壊れ画像でヘッダから寸法が取れない場合（要件12.2 寸法プリチェックの前提崩れ）。
        assert_eq!(
            decide_image_open(None),
            ImageOpenDecision::DimensionsUnknown
        );
    }

    #[test]
    fn 読み取り専用は編集可でベースライン取得可() {
        let p = degrade_for_edge(FsEdge::ReadOnly);
        assert!(p.allow_edit);
        assert!(p.take_baseline);
        assert!(!p.empty_workspace);
    }

    #[test]
    fn クラウドプレースホルダはベースライン取得から除外する() {
        // 要件12.1: フォルダを開いただけで全 DL される事故を防ぐ。
        let p = degrade_for_edge(FsEdge::CloudPlaceholder);
        assert!(!p.take_baseline);
        assert!(p.allow_edit);
    }

    #[test]
    fn ネットワークドライブはポーリングへフォールバックする() {
        let p = degrade_for_edge(FsEdge::NetworkDrive);
        assert!(p.poll_fallback);
        assert!(p.take_baseline);
    }

    #[test]
    fn ワークスペース消失は空状態へ安全遷移する() {
        let p = degrade_for_edge(FsEdge::WorkspaceGone);
        assert!(p.empty_workspace);
        assert!(!p.allow_edit);
    }

    #[test]
    fn アクセス権なしは縮退するがクラッシュさせない方針() {
        // 機能は切るがアプリは継続（編集/ベースライン不可・空状態化はしない）。
        let p = degrade_for_edge(FsEdge::AccessDenied);
        assert!(!p.allow_edit);
        assert!(!p.take_baseline);
        assert!(!p.empty_workspace);
    }
}
