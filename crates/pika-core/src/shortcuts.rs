//! 主要ショートカットのキーディスパッチ表（要件11.2・design doc 19章 主要ショートカット表）。
//!
//! 本モジュールは UI/Tauri/wry/DOM を一切知らない**純粋ロジック**（cargo test の決定論ゲート対象）。
//! 「いまのフォーカスでこのキーは何の操作を発火するか」を**決定論で解決**し、フロントのキーハンドラは
//! 結果（[`Action`]）に従って配線するだけにする。誤爆防止（Ctrl+Enter）と代替割当を表で固める。
//!
//! 要件11.2 の核:
//! - **Ctrl+Enter** はフォーカスが差分/プレビューにあるときだけ「確認済みにする」を発火し、
//!   エディタ編集中の誤爆を防ぐ（エディタフォーカス時は **Ctrl+Shift+Enter**）。
//! - **F8 系**（次/前の変更）・**Ctrl+\\** は他ツール/IME/JIS と衝突しうるため代替割当を併記する
//!   （Alt+Down / Alt+Up、Ctrl+Shift+E）。

/// 修飾キーの状態（フロントの KeyboardEvent から組み立てる）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Mods {
    pub ctrl: bool,
    pub shift: bool,
    pub alt: bool,
}

impl Mods {
    pub fn ctrl() -> Self {
        Mods {
            ctrl: true,
            ..Default::default()
        }
    }
    pub fn ctrl_shift() -> Self {
        Mods {
            ctrl: true,
            shift: true,
            ..Default::default()
        }
    }
    pub fn ctrl_alt() -> Self {
        Mods {
            ctrl: true,
            alt: true,
            ..Default::default()
        }
    }
    pub fn alt() -> Self {
        Mods {
            alt: true,
            ..Default::default()
        }
    }
}

/// いまフォーカスのあるペイン（Ctrl+Enter 誤爆防止に使う＝要件11.2）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Focus {
    /// エディタ（CM6）。Ctrl+Enter で確認済みを発火させない（誤爆防止）。
    Editor,
    /// 差分ビュー。Ctrl+Enter で確認済みを発火してよい。
    Diff,
    /// プレビュー別WebView。Ctrl+Enter で確認済みを発火してよい。
    Preview,
    /// ツリー。
    Tree,
    /// その他（ツールバー等）。
    Other,
}

/// 発火する操作（要件11.2 の機能と対応）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    /// ファイルを開く（Ctrl+O）。
    OpenFile,
    /// フォルダを開く（Ctrl+Shift+O）。
    OpenFolder,
    /// ソース⇔プレビュー切替（Ctrl+E）。
    TogglePreview,
    /// 分割表示（Ctrl+\\・代替 Ctrl+Shift+E）。
    ToggleSplit,
    /// 差分トグル（Ctrl+Shift+D）。
    ToggleDiff,
    /// 確認済みにする（Ctrl+Enter〔差分/プレビュー時〕・Ctrl+Shift+Enter〔フォーカス非依存〕）。
    ConfirmFile,
    /// すべて確認済みにする（Ctrl+Alt+Enter）。
    ConfirmAll,
    /// 次の変更へ（F8・代替 Alt+Down）。
    NextChange,
    /// 前の変更へ（Shift+F8・代替 Alt+Up）。
    PrevChange,
    /// 検索（Ctrl+F）。
    Find,
    /// 置換（Ctrl+H）。
    Replace,
    /// 保存（Ctrl+S）。
    Save,
    /// タブを閉じる（Ctrl+W）。
    CloseTab,
    /// 全体再同期（F5）。
    Resync,
    /// 行へ移動（Ctrl+G）。対話的に行番号を入力してジャンプする（要件5.5）。
    GotoLine,
    /// 次のタブへ（Ctrl+Tab・代替 Ctrl+PageDown〔WebView2 が Ctrl+Tab を飲む環境向け〕・要件11.2）。
    NextTab,
    /// 前のタブへ（Ctrl+Shift+Tab・代替 Ctrl+PageUp・要件11.2）。
    PrevTab,
}

/// キー（修飾＋フォーカス）から発火する操作を解決する（要件11.2）。
///
/// - `key`: KeyboardEvent.key 相当（"Enter"/"F8"/"e"/"\\" 等。英字は小文字で渡す規約）。
/// - `mods`: 修飾キーの状態。
/// - `focus`: 現在フォーカスのあるペイン（Ctrl+Enter 誤爆防止に使う）。
///
/// 一致が無ければ `None`（フロントは既定処理＝CM6 等へ委ねる）。
pub fn resolve(key: &str, mods: Mods, focus: Focus) -> Option<Action> {
    let plain_ctrl = mods.ctrl && !mods.shift && !mods.alt;
    let ctrl_shift = mods.ctrl && mods.shift && !mods.alt;
    let ctrl_alt = mods.ctrl && !mods.shift && mods.alt;
    let plain_alt = !mods.ctrl && mods.alt && !mods.shift;
    let no_mods = !mods.ctrl && !mods.shift && !mods.alt;
    let shift_only = !mods.ctrl && mods.shift && !mods.alt;

    match (key, plain_ctrl, ctrl_shift, ctrl_alt) {
        ("o", true, _, _) => return Some(Action::OpenFile),
        ("o", _, true, _) => return Some(Action::OpenFolder),
        ("e", true, _, _) => return Some(Action::TogglePreview),
        // Ctrl+Shift+E は分割表示の代替（要件11.2）。
        ("e", _, true, _) => return Some(Action::ToggleSplit),
        ("\\", true, _, _) => return Some(Action::ToggleSplit),
        ("d", _, true, _) => return Some(Action::ToggleDiff),
        ("f", true, _, _) => return Some(Action::Find),
        ("h", true, _, _) => return Some(Action::Replace),
        ("s", true, _, _) => return Some(Action::Save),
        ("w", true, _, _) => return Some(Action::CloseTab),
        // 行へ移動（Ctrl+G・要件5.5/11.2）。フロントは対話的に行番号を入力してジャンプする。
        ("g", true, _, _) => return Some(Action::GotoLine),
        // タブ切替（Ctrl+Tab / Ctrl+Shift+Tab・要件11.2）。Ctrl+Tab は WebView2 が飲む可能性があるため
        // 代替に Ctrl+PageDown/Ctrl+PageUp も同じ Action へ割り当てる（系統C で Ctrl+Tab 到達性を確認）。
        ("Tab", true, _, _) => return Some(Action::NextTab),
        ("Tab", _, true, _) => return Some(Action::PrevTab),
        ("PageDown", true, _, _) => return Some(Action::NextTab),
        ("PageUp", true, _, _) => return Some(Action::PrevTab),
        _ => {}
    }

    // Enter 系（確認済み）の誤爆防止（要件11.2）。
    if key == "Enter" {
        // すべて確認済み（Ctrl+Alt+Enter・フォーカス非依存）。
        if ctrl_alt {
            return Some(Action::ConfirmAll);
        }
        // 確認済み（Ctrl+Shift+Enter・フォーカス非依存）。
        if ctrl_shift {
            return Some(Action::ConfirmFile);
        }
        // 確認済み（Ctrl+Enter）は差分/プレビューにフォーカスがあるときだけ発火（エディタ編集中の誤爆防止）。
        if plain_ctrl {
            return match focus {
                Focus::Diff | Focus::Preview => Some(Action::ConfirmFile),
                // エディタ等にフォーカスがあるときは Ctrl+Enter を確認済みに使わない（改行等を尊重）。
                _ => None,
            };
        }
        return None;
    }

    // F8 系（次/前の変更）と代替割当（要件11.2）。
    if key == "F8" {
        if shift_only {
            return Some(Action::PrevChange);
        }
        if no_mods {
            return Some(Action::NextChange);
        }
    }
    if key == "F5" && no_mods {
        return Some(Action::Resync);
    }
    // 代替: Alt+Down / Alt+Up（F8系の JIS/IME 衝突回避＝要件11.2）。
    if plain_alt {
        match key {
            "ArrowDown" => return Some(Action::NextChange),
            "ArrowUp" => return Some(Action::PrevChange),
            _ => {}
        }
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ctrl_e_はプレビュー切替() {
        assert_eq!(
            resolve("e", Mods::ctrl(), Focus::Editor),
            Some(Action::TogglePreview)
        );
    }

    #[test]
    fn ctrl_バックスラッシュと_ctrl_shift_e_は両方分割表示() {
        // 要件11.2: Ctrl+\\ 代替 Ctrl+Shift+E。
        assert_eq!(
            resolve("\\", Mods::ctrl(), Focus::Editor),
            Some(Action::ToggleSplit)
        );
        assert_eq!(
            resolve("e", Mods::ctrl_shift(), Focus::Editor),
            Some(Action::ToggleSplit)
        );
    }

    #[test]
    fn ctrl_shift_d_は差分トグル() {
        assert_eq!(
            resolve("d", Mods::ctrl_shift(), Focus::Editor),
            Some(Action::ToggleDiff)
        );
    }

    #[test]
    fn ctrl_enter_は差分プレビュー時のみ確認済みを発火する() {
        // 要件11.2 誤爆防止: 差分/プレビューフォーカス時のみ。
        assert_eq!(
            resolve("Enter", Mods::ctrl(), Focus::Diff),
            Some(Action::ConfirmFile)
        );
        assert_eq!(
            resolve("Enter", Mods::ctrl(), Focus::Preview),
            Some(Action::ConfirmFile)
        );
    }

    #[test]
    fn ctrl_enter_はエディタフォーカス時は確認済みを発火しない_誤爆防止() {
        // 要件11.2: エディタ編集中は Ctrl+Enter で確認済みを発火させない。
        assert_eq!(resolve("Enter", Mods::ctrl(), Focus::Editor), None);
        assert_eq!(resolve("Enter", Mods::ctrl(), Focus::Tree), None);
    }

    #[test]
    fn ctrl_shift_enter_はフォーカス非依存で確認済み() {
        // エディタフォーカスでも発火する（Ctrl+Enter の代替＝要件11.2）。
        assert_eq!(
            resolve("Enter", Mods::ctrl_shift(), Focus::Editor),
            Some(Action::ConfirmFile)
        );
    }

    #[test]
    fn ctrl_alt_enter_はすべて確認済み() {
        assert_eq!(
            resolve("Enter", Mods::ctrl_alt(), Focus::Editor),
            Some(Action::ConfirmAll)
        );
    }

    #[test]
    fn f8_系は次前の変更で代替割当も効く() {
        // 要件11.2: F8/Shift+F8 と代替 Alt+Down/Alt+Up。
        assert_eq!(
            resolve("F8", Mods::default(), Focus::Diff),
            Some(Action::NextChange)
        );
        let shift = Mods {
            shift: true,
            ..Default::default()
        };
        assert_eq!(resolve("F8", shift, Focus::Diff), Some(Action::PrevChange));
        assert_eq!(
            resolve("ArrowDown", Mods::alt(), Focus::Diff),
            Some(Action::NextChange)
        );
        assert_eq!(
            resolve("ArrowUp", Mods::alt(), Focus::Diff),
            Some(Action::PrevChange)
        );
    }

    #[test]
    fn ctrl_o_と_ctrl_shift_o_は開く系() {
        assert_eq!(
            resolve("o", Mods::ctrl(), Focus::Other),
            Some(Action::OpenFile)
        );
        assert_eq!(
            resolve("o", Mods::ctrl_shift(), Focus::Other),
            Some(Action::OpenFolder)
        );
    }

    #[test]
    fn 基本操作_保存検索置換閉じる再同期() {
        assert_eq!(
            resolve("s", Mods::ctrl(), Focus::Editor),
            Some(Action::Save)
        );
        assert_eq!(
            resolve("f", Mods::ctrl(), Focus::Editor),
            Some(Action::Find)
        );
        assert_eq!(
            resolve("h", Mods::ctrl(), Focus::Editor),
            Some(Action::Replace)
        );
        assert_eq!(
            resolve("w", Mods::ctrl(), Focus::Editor),
            Some(Action::CloseTab)
        );
        assert_eq!(
            resolve("F5", Mods::default(), Focus::Tree),
            Some(Action::Resync)
        );
    }

    #[test]
    fn 未割当キーは_none() {
        assert_eq!(resolve("z", Mods::ctrl(), Focus::Editor), None);
        assert_eq!(resolve("q", Mods::default(), Focus::Editor), None);
    }

    #[test]
    fn ctrl_g_は行へ移動() {
        // 要件5.5/11.2: Ctrl+G で対話的「行へ移動」。フォーカス非依存（フロントが canSearch でガード）。
        assert_eq!(
            resolve("g", Mods::ctrl(), Focus::Editor),
            Some(Action::GotoLine)
        );
        // Ctrl+Shift+G・Ctrl+Alt+G は未割当（誤爆させない）。
        assert_eq!(resolve("g", Mods::ctrl_shift(), Focus::Editor), None);
        assert_eq!(resolve("g", Mods::ctrl_alt(), Focus::Editor), None);
    }

    #[test]
    fn ctrl_tab_系はタブ切替で代替割当も効く() {
        // 要件11.2: Ctrl+Tab=次タブ／Ctrl+Shift+Tab=前タブ。WebView2 が Ctrl+Tab を飲む環境向けに
        // 代替 Ctrl+PageDown/Ctrl+PageUp も同じ Action を返す。
        assert_eq!(
            resolve("Tab", Mods::ctrl(), Focus::Editor),
            Some(Action::NextTab)
        );
        assert_eq!(
            resolve("Tab", Mods::ctrl_shift(), Focus::Editor),
            Some(Action::PrevTab)
        );
        assert_eq!(
            resolve("PageDown", Mods::ctrl(), Focus::Editor),
            Some(Action::NextTab)
        );
        assert_eq!(
            resolve("PageUp", Mods::ctrl(), Focus::Editor),
            Some(Action::PrevTab)
        );
        // 修飾なしの Tab/PageDown/PageUp は CM6/既定処理へ委ねる（None）。
        assert_eq!(resolve("Tab", Mods::default(), Focus::Editor), None);
        assert_eq!(resolve("PageDown", Mods::default(), Focus::Editor), None);
        assert_eq!(resolve("PageUp", Mods::default(), Focus::Editor), None);
    }

    #[test]
    fn 既存割当はs3追加で不変_回帰() {
        // S3 で Action を 3 つ足したが、既存キー割当は一切変わらないことを固定する（パリティ契約の回帰防止）。
        assert_eq!(
            resolve("o", Mods::ctrl(), Focus::Editor),
            Some(Action::OpenFile)
        );
        assert_eq!(
            resolve("e", Mods::ctrl(), Focus::Editor),
            Some(Action::TogglePreview)
        );
        assert_eq!(
            resolve("\\", Mods::ctrl(), Focus::Editor),
            Some(Action::ToggleSplit)
        );
        assert_eq!(
            resolve("d", Mods::ctrl_shift(), Focus::Editor),
            Some(Action::ToggleDiff)
        );
        assert_eq!(
            resolve("s", Mods::ctrl(), Focus::Editor),
            Some(Action::Save)
        );
        assert_eq!(
            resolve("w", Mods::ctrl(), Focus::Editor),
            Some(Action::CloseTab)
        );
        // Ctrl+Enter の誤爆防止（フォーカス依存）も不変。
        assert_eq!(resolve("Enter", Mods::ctrl(), Focus::Editor), None);
        assert_eq!(
            resolve("Enter", Mods::ctrl(), Focus::Diff),
            Some(Action::ConfirmFile)
        );
    }
}
