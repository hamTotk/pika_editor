//! 差分計算（要件8.2・design doc 7章）。
//!
//! 本モジュールは UI/Tauri/wry/notify を一切知らない純粋ロジック（cargo test の決定論ゲート対象）。
//! Rust 側で算出した unified hunk をフロントが DOM 描画する（design doc 7章の責務線引き）:
//!
//! - **行差分** — [`similar`] の Myers LCS（標準機能）。比較は **LF 正規化後**の内容で行い、
//!   改行コードのみの差は差分に出さない（要件8.1）。
//! - **語/grapheme 単位の行内ハイライト** — 変更行（replace）に対し、空白区切りの語境界が
//!   取れる行は語単位、**語境界が取れない行（日本語等）は grapheme 単位へフォールバック**する
//!   判定を自前で行う（design doc 7章「自前なのは語境界不成立行→grapheme単位へ切替える判定のみ」）。
//! - 結果はフロントの read-only unified レンダラ（行頭±記号・変更語の下線/太字・色非依存）が描画する。
//!
//! モジュール構成:
//! - [`line`] — LF 正規化・行分割・行 LCS（[`DiffLine`]/[`FileDiff`]）。
//! - [`inline`] — 変更行ペアの語/grapheme フォールバック行内差分（[`Segment`]）。
//! - [`search_map`] — 差分ビュー内検索のヒット位置マッピング（連結表示テキスト上の byte ヒットを
//!   差分行・セグメント単位の強調範囲へ写す純粋変換。フロント `src/diff/index.ts` の写しの正本＝S5）。

pub mod inline;
pub mod line;
pub mod search_map;

pub use inline::{intra_line_segments, Granularity, Segment};
pub use line::{compute_diff, normalize_lf, DiffLine, DiffTag, FileDiff};
pub use search_map::{diff_display_text, map_matches_to_spans, DiffHitSpan};
