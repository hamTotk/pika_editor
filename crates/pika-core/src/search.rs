//! 検索/置換（要件5.4・design doc 2章/12章/15章-8）。
//!
//! 本モジュールは UI/Tauri/wry を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。
//! 正規表現エンジンは **fancy-regex（第一候補・純Rust）**で、要件5.4 が要求する
//! **後方参照・キャプチャ参照・Unicode 文字クラス**をサポートする（Scintilla 内蔵正規表現は機能が
//! 貧弱なため不採用＝要件5.4）。
//!
//! ReDoS 対策（要件5.4・design doc 12章「ReDoS タイムアウト」）:
//! - fancy-regex の **`backtrack_limit`**（既定 100 万）で 1 マッチあたりのバックトラック爆発を上限化。
//! - 全体処理（全ヒット走査・全置換）は **協調キャンセル**（[`Cancel`] フラグ）＋**マッチ件数上限**で
//!   打ち切る。フロントは長時間処理をキャンセルできる（要件5.4「キャンセル可能」）。
//! - 本モジュールはスレッドを持たない。src-tauri が別スレッドで回し（UI を 200ms 超ブロックしない＝
//!   design doc 3章）、[`Cancel`] を共有して中断する。
//!
//! 検索オプション（要件5.4）: 大文字小文字区別・単語単位・正規表現。リテラル検索は正規表現
//! メタ文字をエスケープして同一経路で扱う（コードを一本化）。

use fancy_regex::Regex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

/// 検索のバックトラック上限（ReDoS 対策・1 マッチあたり）。fancy-regex の既定と同値を明示。
///
/// この上限を超えると fancy-regex は `BacktrackLimitExceeded` を返す。pika は当該パターンを
/// 「危険（ReDoS の疑い）」として扱い、検索結果なし＋エラー理由を返す（無限ハングを防ぐ）。
pub const DEFAULT_BACKTRACK_LIMIT: usize = 1_000_000;

/// 全体処理のマッチ件数上限（巨大ファイルで結果が爆発しても固まらないための安全弁）。
///
/// これを超えるヒットがある場合は打ち切り、[`SearchResult::truncated`] を立てる（要件5.4
/// 「巨大ファイルでも UI をブロックしない」）。フロントは「上限超のため一部のみ表示」を提示する。
pub const DEFAULT_MAX_MATCHES: usize = 100_000;

/// 協調キャンセルのフラグ（src-tauri が別スレッドの検索/置換を止めるために共有する）。
///
/// 長時間処理（巨大ファイルの全ヒット走査・全置換）の途中で [`Cancel::cancel`] を呼ぶと、
/// 次のマッチ境界でループを抜ける（要件5.4「キャンセル可能」）。
#[derive(Debug, Clone, Default)]
pub struct Cancel(Arc<AtomicBool>);

impl Cancel {
    /// 新規（未キャンセル）。
    pub fn new() -> Self {
        Cancel(Arc::new(AtomicBool::new(false)))
    }
    /// キャンセルを要求する（別スレッドから呼んでよい）。
    pub fn cancel(&self) {
        self.0.store(true, Ordering::SeqCst);
    }
    /// キャンセル済みか。
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }
}

/// 検索オプション（要件5.4: 大文字小文字区別・単語単位・正規表現）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SearchOptions {
    /// 大文字小文字を区別するか（false でケースインセンシティブ）。
    pub case_sensitive: bool,
    /// 単語単位か（`\b` で囲んで部分一致を防ぐ）。
    pub whole_word: bool,
    /// 正規表現として解釈するか（false ならリテラル＝メタ文字をエスケープ）。
    pub regex: bool,
}

impl Default for SearchOptions {
    fn default() -> Self {
        SearchOptions {
            case_sensitive: true,
            whole_word: false,
            regex: true,
        }
    }
}

/// 1 ヒットの位置（バイトオフセット・半開区間）。CM6 のハイライト/選択に使う。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Match {
    /// 開始バイト（含む）。
    pub start: usize,
    /// 終了バイト（含まない）。
    pub end: usize,
}

/// 検索結果（要件5.4: ヒット件数・全ヒット位置）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SearchResult {
    /// 全ヒット（上限/キャンセルで打ち切られることがある）。
    pub matches: Vec<Match>,
    /// 件数上限で打ち切られたか（フロントは「一部のみ」を提示）。
    pub truncated: bool,
    /// キャンセルで打ち切られたか。
    pub cancelled: bool,
}

/// 検索/置換のエラー（要件5.4・コア公開 API は Result）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SearchError {
    /// パターンのコンパイルに失敗（不正な正規表現・フロントへ文言提示）。
    InvalidPattern(String),
    /// 実行時にバックトラック上限超過（ReDoS の疑い・当該パターンを危険として弾く）。
    Backtrack,
    /// 置換テンプレートが不正（$名 の参照不正など）。
    InvalidReplacement(String),
}

impl std::fmt::Display for SearchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SearchError::InvalidPattern(m) => write!(f, "不正な検索パターン: {m}"),
            SearchError::Backtrack => write!(
                f,
                "検索が複雑すぎて打ち切りました（ReDoS の疑い）。パターンを見直してください"
            ),
            SearchError::InvalidReplacement(m) => write!(f, "不正な置換テンプレート: {m}"),
        }
    }
}

impl std::error::Error for SearchError {}

/// 検索オプションからパターン文字列を組み立てる（リテラル/単語単位/大小無視を反映）。
///
/// - `regex=false`: メタ文字をエスケープしてリテラル化（同一経路で扱う）。
/// - `whole_word=true`: `\b...\b` で囲む。
/// - `case_sensitive=false`: 先頭に `(?i)` フラグを付ける（fancy-regex のインラインフラグ）。
fn build_pattern(query: &str, opts: SearchOptions) -> String {
    let core = if opts.regex {
        query.to_string()
    } else {
        fancy_regex::escape(query).into_owned()
    };
    let core = if opts.whole_word {
        format!(r"\b(?:{core})\b")
    } else {
        core
    };
    if opts.case_sensitive {
        core
    } else {
        format!("(?i){core}")
    }
}

/// パターンをコンパイルする（backtrack 上限を設定し ReDoS を上限化）。
fn compile(query: &str, opts: SearchOptions) -> Result<Regex, SearchError> {
    let pattern = build_pattern(query, opts);
    let mut builder = fancy_regex::RegexBuilder::new(&pattern);
    builder.backtrack_limit(DEFAULT_BACKTRACK_LIMIT);
    builder
        .build()
        .map_err(|e| SearchError::InvalidPattern(e.to_string()))
}

/// 全ヒットを検索する（要件5.4: 全ヒットのハイライト・件数表示）。
///
/// - 空マッチ（`a*` 等）が無限ループにならないよう、空マッチ時は次の char 境界へ進める。
/// - 件数 [`DEFAULT_MAX_MATCHES`] 到達で打ち切り（`truncated`）。
/// - `cancel` がキャンセルされたら次のマッチ境界で抜ける（`cancelled`）。
/// - `backtrack_limit` 超過は [`SearchError::Backtrack`]（危険パターンを弾く）。
pub fn search_all(
    haystack: &str,
    query: &str,
    opts: SearchOptions,
    cancel: &Cancel,
) -> Result<SearchResult, SearchError> {
    let mut matches = Vec::new();
    let mut truncated = false;
    let mut cancelled = false;
    if query.is_empty() {
        return Ok(SearchResult {
            matches,
            truncated,
            cancelled,
        });
    }
    let re = compile(query, opts)?;
    let mut pos = 0usize;
    while pos <= haystack.len() {
        if cancel.is_cancelled() {
            cancelled = true;
            break;
        }
        match re.find_from_pos(haystack, pos) {
            Ok(Some(m)) => {
                let (start, end) = (m.start(), m.end());
                matches.push(Match { start, end });
                if matches.len() >= DEFAULT_MAX_MATCHES {
                    truncated = true;
                    break;
                }
                // 次の探索開始位置を進める。空マッチ（end==start）は char 境界へ前進する。
                let next = if end > pos {
                    end
                } else {
                    next_char_boundary(haystack, pos)
                };
                // 前進しなければ末尾の空マッチを拾い切った＝終了（無限ループ防止）。
                if next <= pos {
                    break;
                }
                pos = next;
            }
            Ok(None) => break,
            Err(fancy_regex::Error::RuntimeError(
                fancy_regex::RuntimeError::BacktrackLimitExceeded,
            )) => {
                return Err(SearchError::Backtrack);
            }
            Err(e) => return Err(SearchError::InvalidPattern(e.to_string())),
        }
    }
    Ok(SearchResult {
        matches,
        truncated,
        cancelled,
    })
}

/// 置換の結果（要件5.4: 1件ずつ／全置換）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReplaceResult {
    /// 置換後テキスト（キャンセル/上限時は途中まで適用した結果）。
    pub text: String,
    /// 置換した件数。
    pub replaced: usize,
    /// 件数上限で打ち切ったか。
    pub truncated: bool,
    /// キャンセルで打ち切ったか。
    pub cancelled: bool,
}

/// 全置換する（要件5.4: 正規表現置換・キャプチャ参照 `$1`/`${name}`）。
///
/// fancy-regex の `Captures::expand` で `$0`/`$1`/`${name}` のキャプチャ参照を展開する
/// （要件5.4「正規表現置換（キャプチャ参照）に対応」）。マッチ件数上限・協調キャンセルは
/// [`search_all`] と同じ規則で打ち切る。
pub fn replace_all(
    haystack: &str,
    query: &str,
    replacement: &str,
    opts: SearchOptions,
    cancel: &Cancel,
) -> Result<ReplaceResult, SearchError> {
    if query.is_empty() {
        return Ok(ReplaceResult {
            text: haystack.to_string(),
            replaced: 0,
            truncated: false,
            cancelled: false,
        });
    }
    let re = compile(query, opts)?;
    let mut out = String::with_capacity(haystack.len());
    let mut last = 0usize;
    let mut pos = 0usize;
    let mut replaced = 0usize;
    let mut truncated = false;
    let mut cancelled = false;
    while pos <= haystack.len() {
        if cancel.is_cancelled() {
            cancelled = true;
            break;
        }
        match re.captures_from_pos(haystack, pos) {
            Ok(Some(caps)) => {
                let whole = caps
                    .get(0)
                    .expect("captures は必ず全体マッチ(group 0)を持つ");
                let (start, end) = (whole.start(), whole.end());
                // マッチ前の地の文をコピーしてから置換テキストを展開する。
                out.push_str(&haystack[last..start]);
                let mut expanded = String::new();
                caps.expand(replacement, &mut expanded);
                out.push_str(&expanded);
                last = end;
                replaced += 1;
                if replaced >= DEFAULT_MAX_MATCHES {
                    truncated = true;
                    break;
                }
                let next = if end > pos {
                    end
                } else {
                    // 空マッチ: 1 文字を地の文として送り、次の境界へ前進する。
                    let nb = next_char_boundary(haystack, pos);
                    if nb > last {
                        out.push_str(&haystack[last..nb]);
                        last = nb;
                    }
                    nb
                };
                // 前進しなければ終了（末尾の空マッチを拾い切った＝無限ループ防止）。
                if next <= pos {
                    break;
                }
                pos = next;
            }
            Ok(None) => break,
            Err(fancy_regex::Error::RuntimeError(
                fancy_regex::RuntimeError::BacktrackLimitExceeded,
            )) => {
                return Err(SearchError::Backtrack);
            }
            Err(e) => return Err(SearchError::InvalidPattern(e.to_string())),
        }
    }
    // 残りの地の文を付ける（打ち切り時も last 以降を保つ＝内容を失わない）。
    out.push_str(&haystack[last..]);
    Ok(ReplaceResult {
        text: out,
        replaced,
        truncated,
        cancelled,
    })
}

/// `pos` 以降で次の char 境界を返す（空マッチで前進するため）。
fn next_char_boundary(s: &str, pos: usize) -> usize {
    if pos >= s.len() {
        return s.len();
    }
    let mut p = pos + 1;
    while p < s.len() && !s.is_char_boundary(p) {
        p += 1;
    }
    p
}

#[cfg(test)]
mod tests {
    use super::*;

    fn opts_regex() -> SearchOptions {
        SearchOptions {
            case_sensitive: true,
            whole_word: false,
            regex: true,
        }
    }

    #[test]
    fn 全ヒットを検索する() {
        let r = search_all("foo bar foo baz foo", "foo", opts_regex(), &Cancel::new()).unwrap();
        assert_eq!(r.matches.len(), 3);
        assert_eq!(r.matches[0], Match { start: 0, end: 3 });
        assert_eq!(r.matches[1], Match { start: 8, end: 11 });
        assert!(!r.truncated && !r.cancelled);
    }

    #[test]
    fn 大小無視検索() {
        let opts = SearchOptions {
            case_sensitive: false,
            ..opts_regex()
        };
        let r = search_all("Foo FOO foo", "foo", opts, &Cancel::new()).unwrap();
        assert_eq!(r.matches.len(), 3);
    }

    #[test]
    fn 単語単位検索は部分一致を除く() {
        let opts = SearchOptions {
            whole_word: true,
            ..opts_regex()
        };
        // "cat" は "cats" の部分一致を拾わない。
        let r = search_all("cat cats category cat", "cat", opts, &Cancel::new()).unwrap();
        assert_eq!(r.matches.len(), 2); // 先頭 "cat" と末尾 "cat" のみ。
    }

    #[test]
    fn リテラル検索はメタ文字をエスケープする() {
        let opts = SearchOptions {
            regex: false,
            ..opts_regex()
        };
        // "a.b" をリテラルで（"." はワイルドカードでない）。"axb" にはヒットしない。
        let r = search_all("a.b axb a.b", "a.b", opts, &Cancel::new()).unwrap();
        assert_eq!(r.matches.len(), 2);
    }

    #[test]
    fn 後方参照を使える() {
        // (\w)\1 = 同じ文字の連続（後方参照・fancy-regex の要件5.4 機能）。
        let r = search_all("aa bb cd ee", r"(\w)\1", opts_regex(), &Cancel::new()).unwrap();
        // "aa" "bb" "ee" の 3 件。
        assert_eq!(r.matches.len(), 3);
    }

    #[test]
    fn unicode文字クラスを使える() {
        // \p{Hiragana} = ひらがな（Unicode 文字クラス・要件5.4）。
        let r = search_all(
            "あいうABCえお",
            r"\p{Hiragana}+",
            opts_regex(),
            &Cancel::new(),
        )
        .unwrap();
        assert_eq!(r.matches.len(), 2); // "あいう" と "えお"。
    }

    #[test]
    fn 不正なパターンはエラー() {
        let r = search_all("text", "(unclosed", opts_regex(), &Cancel::new());
        assert!(matches!(r, Err(SearchError::InvalidPattern(_))));
    }

    #[test]
    fn 空クエリは結果なし() {
        let r = search_all("text", "", opts_regex(), &Cancel::new()).unwrap();
        assert!(r.matches.is_empty());
    }

    #[test]
    fn 空マッチでも無限ループしない() {
        // "a*" は空マッチを生む。全 char 境界で 1 件ずつ進み、停止する（無限ループしないことが要点）。
        let r = search_all("xyz", "a*", opts_regex(), &Cancel::new()).unwrap();
        assert!(!r.matches.is_empty());
        // 文字数（3）+ 末尾位置（1）の範囲内で停止する（位置ごとに高々 1 件＝有限）。
        assert!(
            r.matches.len() <= "xyz".len() + 1,
            "件数={}",
            r.matches.len()
        );
        assert!(!r.cancelled && !r.truncated);
    }

    #[test]
    fn キャンセルすると打ち切る() {
        let cancel = Cancel::new();
        cancel.cancel(); // 開始前にキャンセル。
        let r = search_all("foo foo foo", "foo", opts_regex(), &cancel).unwrap();
        assert!(r.cancelled);
        assert!(r.matches.is_empty());
    }

    #[test]
    fn キャプチャ参照で全置換する() {
        // 要件5.6 受け入れ基準「正規表現＋キャプチャ参照で全置換」。
        let r = replace_all(
            "2026-06-21 and 2025-01-02",
            r"(\d{4})-(\d{2})-(\d{2})",
            "$3/$2/$1",
            opts_regex(),
            &Cancel::new(),
        )
        .unwrap();
        assert_eq!(r.text, "21/06/2026 and 02/01/2025");
        assert_eq!(r.replaced, 2);
    }

    #[test]
    fn 名前付きキャプチャ参照で全置換する() {
        let r = replace_all(
            "key=value",
            r"(?<k>\w+)=(?<v>\w+)",
            "${v}:${k}",
            opts_regex(),
            &Cancel::new(),
        )
        .unwrap();
        assert_eq!(r.text, "value:key");
        assert_eq!(r.replaced, 1);
    }

    #[test]
    fn リテラル置換はメタ文字を素通しする() {
        let opts = SearchOptions {
            regex: false,
            ..opts_regex()
        };
        let r = replace_all("a.b.c", ".", "_", opts, &Cancel::new()).unwrap();
        assert_eq!(r.text, "a_b_c"); // "." をリテラルで 2 件置換。
        assert_eq!(r.replaced, 2);
    }

    #[test]
    fn 置換でヒットなしは原文のまま() {
        let r = replace_all("hello", "xyz", "ZZZ", opts_regex(), &Cancel::new()).unwrap();
        assert_eq!(r.text, "hello");
        assert_eq!(r.replaced, 0);
    }

    #[test]
    fn 置換のキャンセルは途中までで内容を失わない() {
        let cancel = Cancel::new();
        cancel.cancel();
        let r = replace_all("foo foo foo", "foo", "X", opts_regex(), &cancel).unwrap();
        assert!(r.cancelled);
        // キャンセル時も原文全体が保たれる（last 以降を付ける＝内容を失わない）。
        assert_eq!(r.text, "foo foo foo");
        assert_eq!(r.replaced, 0);
    }

    #[test]
    fn redos危険パターンはバックトラック上限で打ち切る() {
        // 典型的な catastrophic backtracking パターン。backtrack_limit で弾く（無限ハングしない）。
        let evil = "(a+)+$";
        let haystack = format!("{}!", "a".repeat(40)); // 末尾不一致で爆発を誘発。
        let r = search_all(&haystack, evil, opts_regex(), &Cancel::new());
        // 弾けたら Backtrack、弾く前に解けたら Ok（どちらでもハングしないことが要点）。
        match r {
            Err(SearchError::Backtrack) => {}
            Ok(_) => {}
            other => panic!("予期しない結果: {:?}", other),
        }
    }

    #[test]
    fn キャンセルは別ハンドルからも効く() {
        // Cancel は Clone で別スレッド共有を模擬（src-tauri の別スレッドキャンセル）。
        let cancel = Cancel::new();
        let handle = cancel.clone();
        handle.cancel();
        assert!(cancel.is_cancelled());
    }
}
