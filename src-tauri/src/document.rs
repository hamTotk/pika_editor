//! 文書 I/O の配線（巨大ファイル段階制・エンコーディング往復・range 読取・検索置換）。
//!
//! sprint 6 の中心。役割（design doc 3章「薄い境界」）:
//! - 段階判定（[`pika_core::huge`]）・エンコーディング往復と保存中断（[`pika_core::encoding`]）・
//!   range 範囲算出（[`pika_core::range`]）・検索置換（[`pika_core::search`]）は**すべて pika-core**
//!   （cargo test 済み）に委ね、ここは FS 読取/書込と DTO 化に徹する（逆参照禁止・ロジックは core）。
//! - 巨大ファイルは CM6 へ全量ロードせず、Rust の range 読取で仮想化ビューアへ渡す（固まらない）。

use pika_core::encoding::{self, SaveOutcome, TextEncoding};
use pika_core::huge::{degrade_flags, FileStage};
use pika_core::range::{align_to_lines, window_around, DEFAULT_WINDOW_BYTES};
use pika_core::search::{self, Cancel, SearchOptions};
use serde::{Deserialize, Serialize};
use std::io::{Read, Seek, SeekFrom};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use tauri::State;

/// エンコーディング DTO（"utf-8" | "utf-16le" | "utf-16be" | "shift_jis"）。フロント round-trip 用。
fn enc_to_dto(e: TextEncoding) -> &'static str {
    match e {
        TextEncoding::Utf8 => "utf-8",
        TextEncoding::Utf16Le => "utf-16le",
        TextEncoding::Utf16Be => "utf-16be",
        TextEncoding::ShiftJis => "shift_jis",
    }
}

fn dto_to_enc(s: &str) -> Result<TextEncoding, String> {
    match s {
        "utf-8" => Ok(TextEncoding::Utf8),
        "utf-16le" => Ok(TextEncoding::Utf16Le),
        "utf-16be" => Ok(TextEncoding::Utf16Be),
        "shift_jis" => Ok(TextEncoding::ShiftJis),
        other => Err(format!("未知のエンコーディング: {other}")),
    }
}

/// 段階制の縮退フラグ DTO（通知バー提示用・要件2.2/11.1）。
#[derive(Debug, Serialize)]
pub struct DegradeFlagsDto {
    pub preview_off: bool,
    pub diff_off: bool,
    pub baseline_content_off: bool,
    pub highlight_off: bool,
    pub wrap_off: bool,
    pub editing_off: bool,
}

/// 巨大ファイル段階 DTO（"normal" | "stage1" | "stage2" | "too-large"）。
fn stage_to_dto(s: FileStage) -> &'static str {
    match s {
        FileStage::Normal => "normal",
        FileStage::Stage1 => "stage1",
        FileStage::Stage2ReadOnly => "stage2",
        FileStage::TooLarge => "too-large",
    }
}

/// エンコーディング判定＋段階制を解決して開く（要件2.2/5.2）。
///
/// - サイズで段階を判定し、第2段階以降（読み取り専用ビューア）/上限超（開けない）は内容を返さず
///   段階だけ返す（CM6 に全量ロードしない＝固まらない）。フロントは仮想化ビューア（range 読取）へ。
/// - 通常/第1段階はエンコーディング判定してデコード済みテキストを返す（編集可能）。
#[derive(Debug, Serialize)]
pub struct OpenedDocument {
    /// 段階（"normal" | "stage1" | "stage2" | "too-large"）。
    pub stage: String,
    /// 縮退フラグ（自動オフされた機能・通知バー用）。
    pub degrade: DegradeFlagsDto,
    /// デコード済みテキスト（第2段階以降/上限超では空・仮想化ビューア or エラー）。
    pub text: String,
    /// 検出エンコーディング（保存時に維持する）。
    pub encoding: String,
    /// BOM があったか（保存時に維持する）。
    pub has_bom: bool,
    /// 改行分類（表示メニュー用・"lf" | "crlf" | "cr" | "mixed" | "none"）。
    pub line_ending: String,
    /// 自動判定に失敗し UTF-8 警告で開いたか（要件5.2）。
    pub decode_warning: bool,
    /// ファイル全体のバイトサイズ（仮想化ビューアの range 計算に使う）。
    pub size_bytes: u64,
}

fn line_ending_dto(le: encoding::LineEnding) -> &'static str {
    match le {
        encoding::LineEnding::Lf => "lf",
        encoding::LineEnding::Crlf => "crlf",
        encoding::LineEnding::Cr => "cr",
        encoding::LineEnding::Mixed => "mixed",
        encoding::LineEnding::None => "none",
    }
}

/// 文書を開く（巨大ファイル段階制＋エンコーディング自動判定・要件2.2/5.2）。
#[tauri::command]
pub fn open_document(path: String) -> Result<OpenedDocument, String> {
    let size = std::fs::metadata(&path)
        .map_err(|e| format!("メタデータ取得に失敗: {e}"))?
        .len();
    let stage = FileStage::from_size(size);

    // 上限超は開かずエラー（要件2.2 上限）。
    if !stage.can_open() {
        // 診断ログ（要件12.3・本文は書かずパス＋要約のみ）。
        crate::diagnostic::record(
            pika_core::diagnostic::LogLevel::Warn,
            "document",
            "open",
            Some(&path),
            &format!("上限超で開けませんでした（{}）", human_bytes(size)),
        );
        return Err(format!(
            "ファイルが大きすぎて開けません（{} 上限）",
            human_bytes(size)
        ));
    }

    // 第2段階（読み取り専用ビューア）は全量を読まず、段階だけ返す（range 読取はフロントが要求）。
    if stage.needs_virtual_viewer() {
        let degrade = degrade_flags(size, "");
        return Ok(OpenedDocument {
            stage: stage_to_dto(stage).into(),
            degrade: to_degrade_dto(degrade),
            text: String::new(),
            encoding: enc_to_dto(TextEncoding::Utf8).into(),
            has_bom: false,
            line_ending: "none".into(),
            decode_warning: false,
            size_bytes: size,
        });
    }

    // 通常/第1段階: 全量読みエンコーディング判定（第1段階でも編集は通常通り＝中心体験死守）。
    let bytes = std::fs::read(&path).map_err(|e| format!("読み込みに失敗: {e}"))?;
    let decoded = encoding::decode(&bytes);
    // 段階制（サイズ）と行長ガード（内容）の両方を反映した縮退フラグ。
    let degrade = degrade_flags(size, &decoded.text);
    Ok(OpenedDocument {
        stage: stage_to_dto(stage).into(),
        degrade: to_degrade_dto(degrade),
        text: decoded.text,
        encoding: enc_to_dto(decoded.encoding).into(),
        has_bom: decoded.has_bom,
        line_ending: line_ending_dto(decoded.line_ending).into(),
        decode_warning: decoded.had_decode_warning,
        size_bytes: size,
    })
}

fn to_degrade_dto(f: pika_core::huge::DegradeFlags) -> DegradeFlagsDto {
    DegradeFlagsDto {
        preview_off: f.preview_off,
        diff_off: f.diff_off,
        baseline_content_off: f.baseline_content_off,
        highlight_off: f.highlight_off,
        wrap_off: f.wrap_off,
        editing_off: f.editing_off,
    }
}

/// エンコーディング保存の結果 DTO（要件5.2/5.6 の保存中断フロー）。
#[derive(Debug, Serialize)]
pub struct SaveResultDto {
    /// "saved" = 保存完了 / "unmappable" = 表現不能文字で中断（選択肢を提示）。
    pub status: String,
    /// 中断時の表現不能文字（[UTF-8で保存/該当文字を確認/キャンセル] の「確認」用）。
    pub unmappable: Vec<UnmappableDto>,
}

/// 表現不能文字 DTO（位置と文字）。
#[derive(Debug, Serialize)]
pub struct UnmappableDto {
    pub ch: String,
    pub char_index: usize,
}

/// エンコーディングを維持して保存する（要件5.2/5.6）。
///
/// 現エンコーディングで表現不能な文字があれば**保存せず**中断し選択肢を返す（無確認の文字欠落を
/// しない＝最上位原則「データを失わない」）。保存はアトミック書込（一時ファイル→置換）。
///
/// - `force_utf8=true`: 「UTF-8で保存」選択肢（要件5.6）。BOM なし UTF-8 で書き出す。
#[tauri::command]
pub fn save_document(
    path: String,
    content: String,
    encoding: String,
    has_bom: bool,
    force_utf8: bool,
    watcher: State<'_, crate::watcher::WatcherService>,
    snapshot: State<'_, crate::snapshot::SnapshotService>,
) -> Result<SaveResultDto, String> {
    let bytes = if force_utf8 {
        encoding::encode_as_utf8(&content)
    } else {
        let enc = dto_to_enc(&encoding)?;
        match encoding::encode_for_save(&content, enc, has_bom) {
            SaveOutcome::Encoded(b) => b,
            SaveOutcome::Unmappable(chars) => {
                // 中断: 表現不能文字を返し保存しない（要件5.6・データを失わない）。
                return Ok(SaveResultDto {
                    status: "unmappable".into(),
                    unmappable: chars
                        .into_iter()
                        .map(|u| UnmappableDto {
                            ch: u.ch.to_string(),
                            char_index: u.char_index,
                        })
                        .collect(),
                });
            }
        }
    };

    // 退避が先（CLAUDE.md 判断ガイド・最上位原則）: 破壊的上書きの前に、ディスク上の現内容に未確認の
    // 外部変更があれば incoming 退避する。退避が取れなければ保存を中断する（外部変更を無退避で失わない）。
    // ディスク不在（新規保存）は退避対象なし。読めた場合のみ退避判定する。
    if let Ok(disk_content) = std::fs::read_to_string(&path) {
        if let Err(e) = snapshot.stash_incoming_before_overwrite(&path, &disk_content, &content) {
            // 退避が取れないまま破壊的上書きをしない（データを失わない）。診断ログへ記録して中断する。
            crate::diagnostic::record(
                pika_core::diagnostic::LogLevel::Error,
                "document",
                "save",
                Some(&path),
                &format!("保存前の incoming 退避に失敗（保存中断）: {e}"),
            );
            return Err(format!("保存前の退避に失敗したため中断しました: {e}"));
        }
    }

    // 自己保存抑制（save_file と同規則）: 書き出すバイト列のハッシュをトークン登録してから書く。
    let saved_hash = pika_core::hashing::hash_normalized_lf(&bytes);
    watcher.register_self_save(&path, &saved_hash);
    atomic_write(&path, &bytes).map_err(|e| {
        crate::diagnostic::record(
            pika_core::diagnostic::LogLevel::Error,
            "document",
            "save",
            Some(&path),
            &format!("アトミック書込に失敗: {e}"),
        );
        format!("保存に失敗: {e}")
    })?;
    Ok(SaveResultDto {
        status: "saved".into(),
        unmappable: Vec::new(),
    })
}

/// アトミック書込（一時ファイル→単一アトミック置換・属性/ACL は OS の置換挙動に委ねる・要件12.1）。
///
/// 同一ディレクトリに一時ファイルを作って書き込み、`fsync` 後に元パスへ**単一の置換 API**で差し替える
/// （途中クラッシュ/電源断でも元ファイルが半端にならない＝最上位原則「データを失わない」）。
///
/// Windows では `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` を使い、既存ファイルを
/// **1 回のシステムコールでアトミックに置換**する（旧実装の `remove_file→rename` 2 段は、間でクラッシュ
/// すると元ファイル消失・新ファイル未配置で対象パスが消える窓があったため廃止＝eval high data 対応）。
/// 置換に失敗したときは一時ファイルを必ず後始末し、元ファイルは触らない（消さない）。
fn atomic_write(path: &str, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    let target = std::path::Path::new(path);
    let dir = target.parent().unwrap_or_else(|| std::path::Path::new("."));
    let file_name = target
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| "pika.tmp".into());
    let tmp = dir.join(format!(".{file_name}.pika.tmp"));
    {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
    }
    if let Err(e) = replace_atomically(&tmp, target) {
        // 置換に失敗したら一時ファイルを後始末し、元ファイルは一切触らない（半端な状態を作らない）。
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    Ok(())
}

/// 一時ファイルを元パスへ単一アトミック操作で置換する（最上位原則「データを失わない」）。
///
/// Windows: `MoveFileExW` 1 呼び出しで既存を置換（クラッシュ窓を作らない）。元が存在しない新規作成も
/// 同 API で成立する（`MOVEFILE_REPLACE_EXISTING` は対象不在でもエラーにしない）。
/// 非 Windows: `std::fs::rename`（POSIX rename は同一ボリュームでアトミック置換）。
#[cfg(windows)]
fn replace_atomically(tmp: &std::path::Path, target: &std::path::Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{
        MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
    };

    fn wide(p: &std::path::Path) -> Vec<u16> {
        p.as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    }
    let from = wide(tmp);
    let to = wide(target);
    // SAFETY: from/to はヌル終端の有効な UTF-16 パス。MoveFileExW は同期 API で所有権を移さない。
    let ok = unsafe {
        MoveFileExW(
            from.as_ptr(),
            to.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if ok == 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(not(windows))]
fn replace_atomically(tmp: &std::path::Path, target: &std::path::Path) -> std::io::Result<()> {
    std::fs::rename(tmp, target)
}

/// 仮想化ビューアの 1 ウィンドウ（行境界整列済みテキスト＋実バイト範囲）。
#[derive(Debug, Serialize)]
pub struct RangeWindowDto {
    /// 整列後の実開始バイト（フロントの相対スクロール換算に使う）。
    pub start: u64,
    /// 整列後の実終了バイト。
    pub end: u64,
    /// ファイル全体のバイトサイズ。
    pub size_bytes: u64,
    /// 範囲のテキスト（UTF-8 lossy・読み取り専用ビューア表示用）。
    pub text: String,
}

/// 巨大ファイル（第2段階）のバイト範囲を読み、行境界に整えて返す（要件2.2 第2段階・design doc 8章）。
///
/// CM6 に全量ロードせず、要求位置近傍の 1 ウィンドウだけを読む（固まらない）。範囲算出・行整列は
/// pika-core（cargo test 済み）。ここは `seek+read` の I/O のみ。**読み取り専用**（編集/保存不可）。
#[tauri::command]
pub fn read_range(
    path: String,
    center: u64,
    window: Option<u64>,
) -> Result<RangeWindowDto, String> {
    let size = std::fs::metadata(&path)
        .map_err(|e| format!("メタデータ取得に失敗: {e}"))?
        .len();
    let win = window.unwrap_or(DEFAULT_WINDOW_BYTES);
    let raw_range = window_around(center, win, size);
    let mut file = std::fs::File::open(&path).map_err(|e| format!("開けません: {e}"))?;
    file.seek(SeekFrom::Start(raw_range.start))
        .map_err(|e| format!("シークに失敗: {e}"))?;
    let mut buf = vec![0u8; raw_range.len() as usize];
    let read = file
        .read(&mut buf)
        .map_err(|e| format!("範囲読取に失敗: {e}"))?;
    buf.truncate(read);
    // 行境界に整える（半端な行を前後から削る）。算出は pika-core。
    let aligned = align_to_lines(raw_range.start, &buf);
    // 整列で削れた分のバイトを切り出す。
    let lead = (aligned.start - raw_range.start) as usize;
    let trail = (raw_range.start + buf.len() as u64).saturating_sub(aligned.end) as usize;
    let slice_end = buf.len().saturating_sub(trail);
    let text = String::from_utf8_lossy(&buf[lead..slice_end]).into_owned();
    Ok(RangeWindowDto {
        start: aligned.start,
        end: aligned.end,
        size_bytes: size,
        text,
    })
}

/// 検索オプション DTO（要件5.4）。
#[derive(Debug, Clone, Copy, Deserialize)]
pub struct SearchOptionsDto {
    pub case_sensitive: bool,
    pub whole_word: bool,
    pub regex: bool,
}

impl From<SearchOptionsDto> for SearchOptions {
    fn from(d: SearchOptionsDto) -> Self {
        SearchOptions {
            case_sensitive: d.case_sensitive,
            whole_word: d.whole_word,
            regex: d.regex,
        }
    }
}

/// 検索 1 ヒット DTO。
#[derive(Debug, Serialize)]
pub struct MatchDto {
    pub start: usize,
    pub end: usize,
}

/// 検索結果 DTO（要件5.4: 件数・全ヒット・上限/キャンセル）。
#[derive(Debug, Serialize)]
pub struct SearchResultDto {
    pub matches: Vec<MatchDto>,
    pub truncated: bool,
    pub cancelled: bool,
}

/// 検索/置換のキャンセルトークン置き場（要件5.4「キャンセル可能」）。
///
/// 同時に走る検索/置換は 1 本（フロントの検索バー）想定。`generation` で世代を進め、古い検索を
/// キャンセルする（新しい検索を始めたら前のをキャンセル＝UI が固まらない）。
#[derive(Default)]
pub struct SearchCancelService {
    current: Mutex<Option<Cancel>>,
    generation: AtomicU64,
}

impl SearchCancelService {
    pub fn new() -> Self {
        Self::default()
    }
    /// 新しい検索/置換のトークンを発行し、直前のものをキャンセルする。
    fn begin(&self) -> Cancel {
        let token = Cancel::new();
        let mut cur = self.current.lock().expect("search cancel mutex");
        if let Some(prev) = cur.replace(token.clone()) {
            prev.cancel();
        }
        self.generation.fetch_add(1, Ordering::SeqCst);
        token
    }
}

/// 全ヒット検索（要件5.4・別スレッドで回す前提。本実装は同期だが Cancel で打切り可能）。
///
/// 検索計算は pika-core::search（cargo test 済み）。大文字小文字区別・単語単位・正規表現・後方参照・
/// キャプチャ参照・Unicode 文字クラスをサポートし、ReDoS はバックトラック上限で弾く。
#[tauri::command]
pub fn search_in_text(
    text: String,
    query: String,
    options: SearchOptionsDto,
    cancel: State<'_, SearchCancelService>,
) -> Result<SearchResultDto, String> {
    let token = cancel.begin();
    let result =
        search::search_all(&text, &query, options.into(), &token).map_err(|e| e.to_string())?;
    Ok(SearchResultDto {
        matches: result
            .matches
            .into_iter()
            .map(|m| MatchDto {
                start: m.start,
                end: m.end,
            })
            .collect(),
        truncated: result.truncated,
        cancelled: result.cancelled,
    })
}

/// 置換結果 DTO（要件5.4: 置換後テキスト・件数）。
#[derive(Debug, Serialize)]
pub struct ReplaceResultDto {
    pub text: String,
    pub replaced: usize,
    pub truncated: bool,
    pub cancelled: bool,
}

/// 全置換（要件5.4: 正規表現置換・キャプチャ参照）。
///
/// 置換計算は pika-core::search（cargo test 済み）。`$1`/`${name}` のキャプチャ参照を展開する。
/// 差分は読み取り専用・第2段階は置換無効（呼び出し側＝フロントが段階で活性制御する）。
#[tauri::command]
pub fn replace_in_text(
    text: String,
    query: String,
    replacement: String,
    options: SearchOptionsDto,
    cancel: State<'_, SearchCancelService>,
) -> Result<ReplaceResultDto, String> {
    let token = cancel.begin();
    let result = search::replace_all(&text, &query, &replacement, options.into(), &token)
        .map_err(|e| e.to_string())?;
    Ok(ReplaceResultDto {
        text: result.text,
        replaced: result.replaced,
        truncated: result.truncated,
        cancelled: result.cancelled,
    })
}

/// バイト数を人間可読に（エラー文言用・"512.0 MB" 等）。
fn human_bytes(n: u64) -> String {
    const UNITS: &[&str] = &["B", "KB", "MB", "GB"];
    let mut v = n as f64;
    let mut i = 0;
    while v >= 1024.0 && i < UNITS.len() - 1 {
        v /= 1024.0;
        i += 1;
    }
    format!("{v:.1} {}", UNITS[i])
}
