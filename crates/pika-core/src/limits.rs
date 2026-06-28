//! 横断的な上限値の中立な単一 source（レイヤリング: どの上位モジュールにも依存しない最下層）。
//!
//! 行長ガード（[`DEFAULT_LONG_LINE_CHARS`]）と画像ピクセル上限（[`DEFAULT_IMAGE_MAX_PIXELS`]）は、
//! 編集系（[`crate::huge`]）・非テキスト判定（[`crate::nontext`]）・描画ガード（[`crate::render::guard`]）
//! の**複数モジュールから参照される**。これらを描画系（render::guard）に置くと「編集系 → 描画系」という
//! 上向き依存が生まれ、レイヤー依存（一方向）が崩れる。そこで定数の正準定義だけを本モジュール
//! （依存ゼロの最下層）へ集約し、各モジュールはここを参照する（二重定義のドリフトも断つ）。
//!
//! **公開名の互換**: 既存の参照点 `crate::render::guard::DEFAULT_IMAGE_MAX_PIXELS` /
//! `DEFAULT_LONG_LINE_CHARS`（src-tauri は `pika_core::render` 経由で import）は、render::guard 側の
//! `pub use crate::limits::*` 再エクスポートで引き続き解決できる（値・公開名は不変）。

/// 1 行の長さガードの既定上限（文字数・要件2.2: 10万字超でハイライト/折返し自動オフ）。
///
/// AI 出力の単一行巨大 JSON/JSONL を検出する基準。参照元:
/// [`crate::huge::LONG_LINE_CHARS`]・[`crate::render::guard::has_long_line`] の呼び出し側。
pub const DEFAULT_LONG_LINE_CHARS: usize = 100_000;

/// 画像の総ピクセル数の既定上限（要件2.2: 6000万px）。これを**超える**とデコードせず誘導。
///
/// 参照元: [`crate::nontext::MAX_IMAGE_PIXELS`]・[`crate::render::guard::check_image_bytes`] の既定。
pub const DEFAULT_IMAGE_MAX_PIXELS: u64 = 60_000_000;
