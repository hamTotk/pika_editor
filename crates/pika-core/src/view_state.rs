//! ビュー別の5状態解決（ui-design 15章・要件の受け入れ基準と対応）。
//!
//! 本モジュールは UI/Tauri/wry/DOM を一切知らない**純粋ロジック**（cargo test の決定論ゲート対象）。
//! 全ビューで Ideal / Empty / Loading / Partial / Error を**決定論で解決**し、フロントは結果に応じて
//! 描画（CTA・進捗・縮退理由・次の一手）する。状態解決のブレを核へ集約し UI 側の if 散乱を防ぐ。
//!
//! ui-design 15章の表:
//! - **Empty は3分岐**で文言を変える（フォルダ未オープン / 検索0件 / 消化後＝要件10章）。
//! - **Partial（機能縮退）**は黙って切らず**理由＋手動再有効化**を提示（要件2.2）。
//! - **Error** は機能縮退してアプリ起動継続＋次の一手（要件2.3/12章）。

use crate::huge::DegradeFlags;

/// ビューの5状態（ui-design 15章）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ViewState {
    /// 通常表示（フォルダを開きプレビュー/差分＝7〜11章の通常表示）。
    Ideal,
    /// 空（行き止まりにせず CTA 1つ＋最近使った項目。3分岐で文言を変える）。
    Empty(EmptyReason),
    /// 読込中（ベースライン取得中・列挙中。percent-done＋件数。UI は非ブロック）。
    Loading {
        /// 完了件数（列挙済み）。
        done: usize,
        /// 全体件数（既知なら）。
        total: Option<usize>,
    },
    /// 機能縮退（10MB超で差分/プレビュー自動オフ等。理由＋手動再有効化を提示）。
    Partial(Vec<DegradeReason>),
    /// エラー（WebView2 不在/アクセス権なし/衝突。機能縮退してアプリ継続＋次の一手）。
    Error(ErrorReason),
}

/// Empty の3分岐（ui-design 15章「3分岐で文言を変える」＝要件10章）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EmptyReason {
    /// フォルダ未オープン（初回起動）。CTA「フォルダを開く」＋最近使った項目。
    NoFolderOpen,
    /// 検索0件（検索したがヒットなし）。CTA「検索条件を変える」。
    SearchNoResults,
    /// 消化後（未読をすべて確認済みにした後）。CTA「最近使った項目から開く」。
    AllConsumed,
}

impl EmptyReason {
    /// 分岐ごとの文言（ui-design 15章「3分岐で文言を変える」）。
    ///
    /// フロントが直接表示する短い日本語。CTA はフロントのボタンで提供する。
    pub fn message(self) -> &'static str {
        match self {
            EmptyReason::NoFolderOpen => "フォルダを開いてください",
            EmptyReason::SearchNoResults => "条件に一致するものがありませんでした",
            EmptyReason::AllConsumed => "未読はありません",
        }
    }
}

/// Partial の縮退理由（要件2.2・「黙って切らず理由＋手動再有効化」）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DegradeReason {
    /// プレビューを自動オフ（巨大ファイル＝要件2.2）。
    PreviewOff,
    /// 差分を自動オフ（巨大ファイル＝要件2.2）。差分トグルは無効化する。
    DiffOff,
    /// ハイライトを自動オフ（巨大ファイル/長行＝要件2.2）。
    HighlightOff,
    /// 折返しを自動オフ（長行＝要件2.2）。
    WrapOff,
    /// 読み取り専用ビューア（第2段階＝編集不可・要件2.2）。
    ReadOnlyViewer,
    /// ベースライン未取得（差分基準を取得中・ui-design 15章 Partial）。
    BaselinePending,
}

impl DegradeReason {
    /// 理由文言＋「手動再有効化できるか」（要件2.2「手動再有効化」）。
    pub fn message(self) -> &'static str {
        match self {
            DegradeReason::PreviewOff => "大きいファイルのためプレビューを自動的に無効にしました",
            DegradeReason::DiffOff => "大きいファイルのため差分を自動的に無効にしました",
            DegradeReason::HighlightOff => "ハイライトを自動的に無効にしました",
            DegradeReason::WrapOff => "長い行のため折り返しを自動的に無効にしました",
            DegradeReason::ReadOnlyViewer => {
                "非常に大きいファイルのため読み取り専用で表示しています"
            }
            DegradeReason::BaselinePending => "差分基準を取得中です",
        }
    }

    /// ユーザーが手動で再有効化できるか（要件2.2）。読み取り専用ビューアは段階制で固定なので不可。
    pub fn can_reenable(self) -> bool {
        !matches!(self, DegradeReason::ReadOnlyViewer)
    }
}

/// Error の理由（要件2.3/12章・「機能縮退してアプリ継続＋次の一手」）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorReason {
    /// WebView2 不在/破損（要件2.3 改訂・design doc 18章）。最小ネイティブダイアログ経路は起動前。
    WebView2Missing,
    /// アクセス権なし（要件12.1）。
    AccessDenied,
    /// 外部変更の衝突（要件7.2）。
    Conflict,
}

impl ErrorReason {
    /// エラー文言＋次の一手（要件12章「次の一手を提示」）。
    pub fn message(self) -> &'static str {
        match self {
            ErrorReason::WebView2Missing => {
                "WebView2 ランタイムが必要です。導入してから再起動してください"
            }
            ErrorReason::AccessDenied => "アクセスできませんでした。権限を確認してください",
            ErrorReason::Conflict => "外部で変更されました。差分を確認してください",
        }
    }
}

/// 縮退フラグ（[`DegradeFlags`]）から Partial の理由列を組み立てる（要件2.2）。
///
/// 何もオフしていなければ空（Partial にしない＝Ideal）。1つでもオフなら理由列を返す。
pub fn degrade_reasons(flags: DegradeFlags) -> Vec<DegradeReason> {
    let mut out = Vec::new();
    if flags.preview_off {
        out.push(DegradeReason::PreviewOff);
    }
    if flags.diff_off {
        out.push(DegradeReason::DiffOff);
    }
    if flags.highlight_off {
        out.push(DegradeReason::HighlightOff);
    }
    if flags.wrap_off {
        out.push(DegradeReason::WrapOff);
    }
    if flags.editing_off {
        out.push(DegradeReason::ReadOnlyViewer);
    }
    out
}

/// エディタ/ツリービューの状態を解決する（ui-design 15章）。
///
/// 優先順位（最も強い状態を返す）: Error ＞ Loading ＞ Partial ＞ Empty ＞ Ideal。
/// - `error`: 何らかのエラーがあれば最優先（アプリ継続＋次の一手）。
/// - `loading`: 列挙/ベースライン取得中なら Loading（非ブロック表示）。
/// - `degrade`: 機能縮退があれば Partial（理由＋手動再有効化）。
/// - `empty`: 表示すべき中身が無ければ Empty（3分岐）。
/// - いずれも無ければ Ideal。
pub fn resolve_view_state(
    error: Option<ErrorReason>,
    loading: Option<(usize, Option<usize>)>,
    degrade: DegradeFlags,
    empty: Option<EmptyReason>,
) -> ViewState {
    if let Some(e) = error {
        return ViewState::Error(e);
    }
    if let Some((done, total)) = loading {
        return ViewState::Loading { done, total };
    }
    let reasons = degrade_reasons(degrade);
    if !reasons.is_empty() {
        return ViewState::Partial(reasons);
    }
    if let Some(reason) = empty {
        return ViewState::Empty(reason);
    }
    ViewState::Ideal
}

#[cfg(test)]
mod tests {
    use super::*;

    fn no_degrade() -> DegradeFlags {
        DegradeFlags::default()
    }

    #[test]
    fn 何も無ければ_ideal() {
        let s = resolve_view_state(None, None, no_degrade(), None);
        assert_eq!(s, ViewState::Ideal);
    }

    #[test]
    fn empty_は3分岐で文言が変わる() {
        // ui-design 15章: フォルダ未オープン / 検索0件 / 消化後。
        assert_ne!(
            EmptyReason::NoFolderOpen.message(),
            EmptyReason::SearchNoResults.message()
        );
        assert_ne!(
            EmptyReason::SearchNoResults.message(),
            EmptyReason::AllConsumed.message()
        );
        let s = resolve_view_state(None, None, no_degrade(), Some(EmptyReason::NoFolderOpen));
        assert_eq!(s, ViewState::Empty(EmptyReason::NoFolderOpen));
    }

    #[test]
    fn エラーは最優先で次の一手を返す() {
        // Error ＞ Loading ＞ Partial ＞ Empty。
        let mut flags = no_degrade();
        flags.preview_off = true;
        let s = resolve_view_state(
            Some(ErrorReason::AccessDenied),
            Some((1, Some(10))),
            flags,
            Some(EmptyReason::NoFolderOpen),
        );
        assert_eq!(s, ViewState::Error(ErrorReason::AccessDenied));
        assert!(ErrorReason::AccessDenied.message().contains("権限"));
    }

    #[test]
    fn loading_は件数と進捗を持つ() {
        let s = resolve_view_state(None, Some((620, Some(1000))), no_degrade(), None);
        assert_eq!(
            s,
            ViewState::Loading {
                done: 620,
                total: Some(1000)
            }
        );
    }

    #[test]
    fn partial_は縮退理由と再有効化可否を持つ() {
        // 要件2.2: 黙って切らず理由＋手動再有効化。
        let mut flags = no_degrade();
        flags.preview_off = true;
        flags.diff_off = true;
        let s = resolve_view_state(None, None, flags, None);
        match s {
            ViewState::Partial(reasons) => {
                assert!(reasons.contains(&DegradeReason::PreviewOff));
                assert!(reasons.contains(&DegradeReason::DiffOff));
                // プレビュー/差分は手動再有効化できる。
                assert!(DegradeReason::PreviewOff.can_reenable());
            }
            _ => panic!("Partial になるはず"),
        }
    }

    #[test]
    fn 読み取り専用ビューアは手動再有効化できない() {
        // 第2段階は段階制で固定（要件2.2）。
        assert!(!DegradeReason::ReadOnlyViewer.can_reenable());
        let mut flags = no_degrade();
        flags.editing_off = true;
        let reasons = degrade_reasons(flags);
        assert!(reasons.contains(&DegradeReason::ReadOnlyViewer));
    }

    #[test]
    fn partial_より_loading_が優先される() {
        let mut flags = no_degrade();
        flags.preview_off = true;
        let s = resolve_view_state(None, Some((1, None)), flags, None);
        assert!(matches!(s, ViewState::Loading { .. }));
    }

    #[test]
    fn 縮退なしなら_partial_理由は空() {
        assert!(degrade_reasons(no_degrade()).is_empty());
    }
}
