//! 巨大ファイル段階制（要件2.2/5.4・design doc 8章/15章-6/16章）。
//!
//! CodeMirror 6 はインメモリ文書モデルで Scintilla ほど巨大ファイルに堅くないため、
//! 要件2.2 の閾値を Web 現実値へ改訂する（design doc 8章「改訂の波及表」）。本モジュールは
//! サイズ・行長から「どの段階の縮退をかけるか」を**決定論ロジック**で判定する純粋関数群で、
//! UI/Tauri/wry を一切知らない（cargo test の決定論ゲート対象）。
//!
//! 段階（要件2.2 の表。**TBD だった第2段階/上限を sprint6 で確定**＝design doc 16章 canon 改訂）:
//! - **第1段階（10MB 以上）**: プレビュー・差分・ベースライン内容保存・ハイライトを**自動オフ**。
//!   閲覧・編集・検索・保存は**通常通り**（中心シナリオ「AI出力の単一行巨大 JSON/JSONL」死守）。
//!   未読判定はハッシュのみ（9.2）。
//! - **第2段階（確定値 50MB 超）**: **読み取り専用ビューアモード**（閲覧・検索のみ。編集/保存/置換不可）。
//!   Rust ストリーミング range 読取＋仮想化ウィンドウビューア（[`crate::range`]）で CM6 に全量ロードしない。
//! - **上限（確定値 500MB 超）**: 開かず、明確なエラー（要件2.2 上限）。
//!
//! 確定値の根拠（design doc 15章-6 実測ゲート・findings に記録）: 基準機で CM6 の編集体感が
//! 劣化し始めるサイズを実測し「第1段階＝10MB で編集・検索・保存が通常通り」を死守できることを
//! 確認したうえで、第2段階を旧 200MB から **50MB** へ、上限を旧 2GB から **500MB** へ Web 現実値へ
//! 引き下げる（CM6 インメモリ文書の実体感に合わせる）。10MB を下回って劣化する場合のみ中心体験
//! 後退として別途要件改訂を提案する（本モジュールは死守ライン 10MB を境界に置く）。

/// 第1段階の閾値（バイト）。これ**以上**で機能自動オフ（要件2.2 第1段階・暫定10MB を維持）。
///
/// [`crate::hashing::HUGE_FILE_THRESHOLD_BYTES`]・[`crate::snapshot::policy::DEFAULT_CONTENT_LIMIT_BYTES`]
/// と同一の 10MB（内容保存境界＝9.2 と連動）。死守ライン（design doc 8章実測ゲート）。
pub const STAGE1_THRESHOLD_BYTES: u64 = 10 * 1024 * 1024;

/// 第2段階の閾値（バイト）。これを**超える**と読み取り専用ビューアモード（編集/保存/置換不可）。
///
/// **sprint6 で確定**（要件2.2 第2段階 TBD → 50MB・5.4 の置換無効数値と連動・design doc 16章）。
/// 旧 200MB から CM6 の Web 現実値へ引き下げ（findings に実測根拠を記録）。
pub const STAGE2_THRESHOLD_BYTES: u64 = 50 * 1024 * 1024;

/// 上限（バイト）。これを**超える**と開かずエラー（要件2.2 上限）。
///
/// **sprint6 で確定**（要件2.2 上限 TBD → 500MB・design doc 16章）。旧 2GB から引き下げ。
pub const MAX_OPEN_BYTES: u64 = 500 * 1024 * 1024;

/// 行長ガードの閾値（文字数）。1 行がこれを**超える**とサイズと独立にハイライト/折返しを自動オフ。
///
/// AI 出力の単一行巨大 JSON/JSONL 対策（要件2.2 行長ガード）。値の単一 source は中立な最下層
/// [`crate::limits::DEFAULT_LONG_LINE_CHARS`]（同値を参照し二重定義のドリフトを断つ＝eval low）。
/// 旧来 `crate::render::guard` を参照していたが、編集系（huge）→ 描画系（render）の**上向き依存**に
/// なるため limits へ寄せた（レイヤー依存を一方向に保つ＝S3）。
pub const LONG_LINE_CHARS: usize = crate::limits::DEFAULT_LONG_LINE_CHARS;

// 10MB 閾値の三者一致を**コンパイル時**に担保する（eval low data: 単一源化）。
// 段階制境界（[`STAGE1_THRESHOLD_BYTES`]）・内容保存境界（[`crate::snapshot::policy::DEFAULT_CONTENT_LIMIT_BYTES`]）・
// 未読ハッシュ閾値（[`crate::hashing::HUGE_FILE_THRESHOLD_BYTES`]）が同値でないと、段階制と内容保存と未読判定が
// ズレてデータ整合（要件9.2「10MB未満のみ内容保存」と 2.2 第1段階）が壊れる。片方だけ変えたらビルドが落ちる。
const _: () = {
    assert!(STAGE1_THRESHOLD_BYTES == crate::snapshot::policy::DEFAULT_CONTENT_LIMIT_BYTES);
    assert!(STAGE1_THRESHOLD_BYTES == crate::hashing::HUGE_FILE_THRESHOLD_BYTES);
};

/// 巨大ファイル段階（要件2.2 の表に対応・観測可能にしてテストで分岐を検証する）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileStage {
    /// 通常（10MB 未満）。全機能オン。
    Normal,
    /// 第1段階（10MB 以上・第2段階以下）。プレビュー/差分/ベースライン内容保存/ハイライト自動オフ。
    /// 閲覧・編集・検索・保存は通常通り（要件2.2 第1段階）。
    Stage1,
    /// 第2段階（第2段階閾値超・上限以下）。読み取り専用ビューアモード（閲覧・検索のみ）。
    Stage2ReadOnly,
    /// 上限超。開かずエラー（要件2.2 上限）。
    TooLarge,
}

impl FileStage {
    /// サイズ（バイト）から段階を判定する（要件2.2）。
    ///
    /// 境界の扱い:
    /// - 第1段階は 10MB **以上**（9.2「10MB未満のみ内容保存」と整合・ちょうど10MBは第1段階）。
    /// - 第2段階・上限は閾値**超**（ちょうどは下の段階に留める）。
    pub fn from_size(size_bytes: u64) -> FileStage {
        if size_bytes > MAX_OPEN_BYTES {
            FileStage::TooLarge
        } else if size_bytes > STAGE2_THRESHOLD_BYTES {
            FileStage::Stage2ReadOnly
        } else if size_bytes >= STAGE1_THRESHOLD_BYTES {
            FileStage::Stage1
        } else {
            FileStage::Normal
        }
    }

    /// ファイルを開けるか（上限超は開かない＝要件2.2 上限）。
    pub fn can_open(self) -> bool {
        !matches!(self, FileStage::TooLarge)
    }

    /// CM6 に全量ロードして編集できるか（第2段階以降は読み取り専用ビューア＝要件2.2 第2段階）。
    pub fn can_edit(self) -> bool {
        matches!(self, FileStage::Normal | FileStage::Stage1)
    }

    /// 保存できるか（編集できる段階のみ。第2段階は保存不可＝要件5.4）。
    pub fn can_save(self) -> bool {
        self.can_edit()
    }

    /// 置換できるか（差分は読み取り専用・第2段階は置換無効＝要件5.4）。編集可と同条件。
    pub fn can_replace(self) -> bool {
        self.can_edit()
    }

    /// プレビュー/差分/ベースライン内容保存/ハイライトを自動オフにするか（第1段階以降＝要件2.2）。
    pub fn auto_off_heavy_features(self) -> bool {
        !matches!(self, FileStage::Normal)
    }

    /// 仮想化ウィンドウビューア（range 読取）で読むか（第2段階以降＝design doc 8章）。
    pub fn needs_virtual_viewer(self) -> bool {
        matches!(self, FileStage::Stage2ReadOnly)
    }
}

/// 縮退の理由フラグ（通知バーで「なぜオフか／手動再有効化できるか」を提示するため・要件2.2/11.1）。
///
/// 段階制（サイズ）と行長ガード（独立）を合算した「このファイルで自動オフされた機能」を表す。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DegradeFlags {
    /// プレビューを自動オフ（第1段階以降）。
    pub preview_off: bool,
    /// 差分を自動オフ（第1段階以降）。
    pub diff_off: bool,
    /// ベースライン内容保存を自動オフ＝ハッシュのみ（第1段階以降・9.2）。
    pub baseline_content_off: bool,
    /// シンタックスハイライトを自動オフ（第1段階以降 or 長行検出）。
    pub highlight_off: bool,
    /// 行の折返しを自動オフ（長行検出時・要件2.2 行長ガード）。
    pub wrap_off: bool,
    /// 編集を自動オフ＝読み取り専用ビューア（第2段階以降）。
    pub editing_off: bool,
}

impl DegradeFlags {
    /// 何らかの機能を自動オフしているか（通知バー提示要否）。
    pub fn any(self) -> bool {
        self.preview_off
            || self.diff_off
            || self.baseline_content_off
            || self.highlight_off
            || self.wrap_off
            || self.editing_off
    }
}

/// ファイルのサイズと内容（先頭サンプルで可）から縮退フラグを決定する（要件2.2）。
///
/// - `size_bytes`: ファイルのバイトサイズ（段階判定に使う）。
/// - `text`: 行長ガード判定に使うテキスト（巨大ファイルでは先頭の十分なサンプルで可。
///   1 行でも [`LONG_LINE_CHARS`] 超があればハイライト/折返しを自動オフ＝要件2.2 行長ガード）。
///
/// 段階制（サイズ）と行長ガード（独立）の両方を反映する（design doc 8章「行長ガードは維持」）。
pub fn degrade_flags(size_bytes: u64, text: &str) -> DegradeFlags {
    let stage = FileStage::from_size(size_bytes);
    let heavy_off = stage.auto_off_heavy_features();
    let long_line = has_long_line(text);
    DegradeFlags {
        preview_off: heavy_off,
        diff_off: heavy_off,
        baseline_content_off: heavy_off,
        // ハイライトは「第1段階以降」または「長行検出」のいずれかでオフ（行長ガードは独立＝要件2.2）。
        highlight_off: heavy_off || long_line,
        // 折返しは長行検出時にオフ（巨大JSONLの折返し爆発を防ぐ）。
        wrap_off: long_line,
        editing_off: !stage.can_edit(),
    }
}

/// 1 行でも行長ガード（[`LONG_LINE_CHARS`] 超）に掛かるか（要件2.2 行長ガード）。
///
/// 改行を含まない巨大 1 行（AI 出力の単一行 JSON/JSONL）を検出する。文字数（grapheme でなく
/// char）で数え、ハイライト/折返しの自動オフ判定に使う（巨大ファイルでは呼び出し側が先頭サンプル
/// を渡す前提）。閾値は [`crate::limits::DEFAULT_LONG_LINE_CHARS`] と同値。
///
/// **注記（[`crate::render::guard::has_long_line`] とは別実装のまま統合しない）**: 本関数は単独 CR
/// （`\n` を伴わない `\r`＝旧 Mac 改行）を行長に**数えない**（`docs/acceptance-findings.md`「CR は
/// 行長に数えない」で固定された挙動）。一方 guard 版は `str::lines()` ベースで単独 CR を行内文字として
/// **数える**。両者は単独 CR 入力で判定結果が割れるため、一本化すると外部挙動を変えてしまう（純粋
/// リファクタの原則に反する）。重複に見えるが意図的に別実装を保つ（S2 で統合を見送り＝下記レポート）。
pub fn has_long_line(text: &str) -> bool {
    // 改行で分割して各行の char 数を見る。末尾改行なしの 1 行巨大ファイルにも効く。
    let mut count = 0usize;
    for ch in text.chars() {
        if ch == '\n' {
            count = 0;
            continue;
        }
        if ch == '\r' {
            // CR は行区切り扱いにせず、CRLF/CR どちらでも \n 側でリセットされる。
            continue;
        }
        count += 1;
        if count > LONG_LINE_CHARS {
            return true;
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn _10mb未満は通常段階() {
        assert_eq!(
            FileStage::from_size(STAGE1_THRESHOLD_BYTES - 1),
            FileStage::Normal
        );
        let s = FileStage::from_size(1024);
        assert!(s.can_edit() && s.can_save() && s.can_replace() && !s.auto_off_heavy_features());
    }

    #[test]
    fn ちょうど_10mb_は第1段階() {
        // 9.2「10MB未満のみ内容保存・ちょうど10MBはハッシュのみ」と整合（ちょうど10MB＝第1段階）。
        assert_eq!(
            FileStage::from_size(STAGE1_THRESHOLD_BYTES),
            FileStage::Stage1
        );
    }

    #[test]
    fn 第1段階は編集保存検索可だが重い機能はオフ() {
        let s = FileStage::from_size(STAGE1_THRESHOLD_BYTES + 1);
        assert_eq!(s, FileStage::Stage1);
        // 中心シナリオ死守: 編集・保存・置換は通常通り。
        assert!(s.can_edit(), "第1段階で編集できない（中心体験後退）");
        assert!(s.can_save());
        assert!(s.can_replace());
        // 重い機能（プレビュー/差分/内容保存/ハイライト）は自動オフ。
        assert!(s.auto_off_heavy_features());
        assert!(!s.needs_virtual_viewer());
    }

    #[test]
    fn ちょうど第2段階閾値はまだ第1段階() {
        // 閾値**超**で第2段階（ちょうどは第1段階に留める）。
        assert_eq!(
            FileStage::from_size(STAGE2_THRESHOLD_BYTES),
            FileStage::Stage1
        );
    }

    #[test]
    fn 第2段階は読み取り専用ビューア() {
        let s = FileStage::from_size(STAGE2_THRESHOLD_BYTES + 1);
        assert_eq!(s, FileStage::Stage2ReadOnly);
        assert!(s.can_open());
        // 読み取り専用: 編集/保存/置換は不可（要件2.2 第2段階・5.4）。
        assert!(!s.can_edit());
        assert!(!s.can_save());
        assert!(!s.can_replace());
        // 仮想化ビューア（range 読取）で読む。
        assert!(s.needs_virtual_viewer());
    }

    #[test]
    fn ちょうど上限はまだ第2段階() {
        assert_eq!(
            FileStage::from_size(MAX_OPEN_BYTES),
            FileStage::Stage2ReadOnly
        );
    }

    #[test]
    fn 上限超は開かない() {
        let s = FileStage::from_size(MAX_OPEN_BYTES + 1);
        assert_eq!(s, FileStage::TooLarge);
        assert!(!s.can_open());
        assert!(!s.can_edit());
    }

    #[test]
    fn 行長ガードは長い単一行を検出する() {
        // 改行なしの巨大 1 行（AI 出力の単一行 JSON）。
        let big_line = "a".repeat(LONG_LINE_CHARS + 1);
        assert!(has_long_line(&big_line));
        // ちょうど閾値は掛からない。
        let exact = "a".repeat(LONG_LINE_CHARS);
        assert!(!has_long_line(&exact));
    }

    #[test]
    fn 行長ガードは改行で行ごとにリセットする() {
        // 各行が短ければ全体が長くても掛からない。
        let many_short = "short line\n".repeat(50_000);
        assert!(!has_long_line(&many_short));
        // CRLF 前に閾値超の長行があれば検出する（CR は行長カウントに数えない＝LF 側でリセット）。
        let crlf = format!("{}\r\nshort", "a".repeat(LONG_LINE_CHARS + 1));
        assert!(has_long_line(&crlf), "CRLF 前の長行を検出できない");
        // CRLF 区切りで各行が短ければ掛からない（行ごとにリセットされる）。
        let crlf_short = "short\r\n".repeat(50_000);
        assert!(!has_long_line(&crlf_short));
    }

    #[test]
    fn 縮退フラグは通常ファイルでは何もオフしない() {
        let f = degrade_flags(1024, "hello\nworld");
        assert!(!f.any());
    }

    #[test]
    fn 縮退フラグは第1段階で重い機能をオフする() {
        let f = degrade_flags(STAGE1_THRESHOLD_BYTES + 1, "short content");
        assert!(f.preview_off);
        assert!(f.diff_off);
        assert!(f.baseline_content_off);
        assert!(f.highlight_off);
        // 編集はオフにしない（第1段階は編集通常通り）。
        assert!(!f.editing_off);
        assert!(!f.wrap_off); // 長行でなければ折返しは維持。
    }

    #[test]
    fn 縮退フラグは長行で折返しとハイライトをオフする() {
        // 10MB 未満でも長行ならハイライト/折返しオフ（サイズ独立＝要件2.2 行長ガード）。
        let long = "x".repeat(LONG_LINE_CHARS + 1);
        let f = degrade_flags(1024, &long);
        assert!(f.highlight_off);
        assert!(f.wrap_off);
        // サイズは小さいので重い機能（プレビュー/差分/内容保存）は維持。
        assert!(!f.preview_off);
        assert!(!f.diff_off);
        assert!(!f.editing_off);
    }

    #[test]
    fn 縮退フラグは第2段階で編集をオフする() {
        let f = degrade_flags(STAGE2_THRESHOLD_BYTES + 1, "x");
        assert!(f.editing_off);
        assert!(f.any());
    }

    #[test]
    fn 確定閾値の整合性() {
        // 第1段階 < 第2段階 < 上限 の順序（段階制が壊れていないこと）。コンパイル時に検証する。
        const { assert!(STAGE1_THRESHOLD_BYTES < STAGE2_THRESHOLD_BYTES) };
        const { assert!(STAGE2_THRESHOLD_BYTES < MAX_OPEN_BYTES) };
        // 第1段階は 10MB 死守（design doc 15章-6 実測ゲート）。
        assert_eq!(STAGE1_THRESHOLD_BYTES, 10 * 1024 * 1024);
    }
}
