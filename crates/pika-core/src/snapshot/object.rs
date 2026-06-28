//! content-addressed object と自己記述メタ（要件9.1・design doc 11章）。
//!
//! 内容実体（object）は **LF 正規化後の内容ハッシュ**でアドレッシングし、同一内容を重複排除・共有する
//! （twox-hash＝XXH3 系の高速ハッシュ。暗号用途ではなく同一性/重複判定用＝design doc 11章）。
//! 退避 object には自己記述メタ（元 relPath・kind・時刻・元 index 世代）を併記し、
//! 索引（index.json）が破損しても object 群の走査から退避一覧を再生成できる（最上位原則1）。

use serde::{Deserialize, Serialize};

/// 退避の種別（要件9.2・7章/8章で定義した退避の発生源）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum StashKind {
    /// 衝突: 未保存編集 vs 外部変更で［取り込む］時に自分の編集を退避（要件7.3）。
    Conflict,
    /// 取り込み相手: 外部変更を上書き保存する直前に上書きされるディスク内容を退避（要件7.3）。
    Incoming,
    /// 巻き戻し: 「確認済み時点に戻す」で失われる現在内容を退避（要件8.3）。
    Rollback,
    /// 一括確認の更新前ベースライン（「すべて確認済み」のバッチ退避＝要件8.3/9.2）。
    BaselineReplace,
}

impl StashKind {
    /// 自己記述メタ/診断用の安定文字列表現（永続化のキーにも使える）。
    pub fn as_str(self) -> &'static str {
        match self {
            StashKind::Conflict => "conflict",
            StashKind::Incoming => "incoming",
            StashKind::Rollback => "rollback",
            StashKind::BaselineReplace => "baseline-replace",
        }
    }
}

/// 退避 object に併記する自己記述メタ（要件9.1）。
///
/// index 破損時はこのメタだけから「復元待ちの退避一覧」を提示できる（最後の砦に到達可能）。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ObjectMeta {
    /// 元の相対パス（ワークスペース基準。単体ファイルはファイルキー）。
    pub rel_path: String,
    /// 退避種別。
    pub kind: StashKind,
    /// 退避時刻（ミリ秒・単調増加）。
    pub created_at_ms: u64,
    /// 退避時の index 世代（破損復元時の整合確認に使う）。
    pub index_generation: u64,
}

/// 既定 zstd 圧縮レベル（軽量配布より復元の確実性・速度を優先＝中庸の 3）。
pub const DEFAULT_ZSTD_LEVEL: i32 = 3;

/// 改行コードを LF 正規化したうえで内容ハッシュを算出する（要件8.1/9.2）。
///
/// 未読判定・差分照合・object アドレッシングを同一のハッシュ規則で揃える
/// （改行のみの差を別 object にしない＝重複排除と整合）。16 桁 16 進文字列。
/// LF 正規化＋XxHash64 の本体は [`crate::hashing::hash_normalized_lf_str`] に集約済み（#40・出力不変）。
pub fn hash_normalized(content: &str) -> String {
    crate::hashing::hash_normalized_lf_str(content)
}

/// 内容を zstd 圧縮する（永続化前の object 圧縮＝要件9.1「圧縮して保存」）。
///
/// `zstd::encode_all` はメモリ上で完結し、失敗は内部 I/O エラーのみ（スライス入力では実質起きない）。
/// それでも `expect` で **panic=abort** させず [`crate::Result`] で返し、呼び出し側（退避の永続化）が
/// 「圧縮できなければ object を書かずに諦める」へ安全に倒せるようにする（最上位原則「データを失わない」
/// ＝最後の砦を堅牢化）。**メモリ zstd は実際には失敗しないため観測挙動は不変**。
pub fn zstd_compress(content: &[u8]) -> crate::Result<Vec<u8>> {
    zstd::encode_all(content, DEFAULT_ZSTD_LEVEL)
        .map_err(|e| crate::PikaError::Internal(format!("zstd 圧縮に失敗: {e}")))
}

/// zstd 圧縮された object を復元する。破損時は `None`（呼び出し側が他 object/メタへフォールバック）。
pub fn zstd_decompress(compressed: &[u8]) -> Option<Vec<u8>> {
    zstd::decode_all(compressed).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 改行のみ違う内容は同じハッシュ() {
        // LF 正規化後ハッシュ＝改行コードのみの差を別 object にしない（重複排除と整合）。
        assert_eq!(hash_normalized("a\r\nb"), hash_normalized("a\nb"));
        assert_eq!(hash_normalized("a\rb"), hash_normalized("a\nb"));
    }

    #[test]
    fn 内容が違えばハッシュも違う() {
        assert_ne!(hash_normalized("hello"), hash_normalized("world"));
    }

    #[test]
    fn zstd_は往復で元に戻る() {
        let original = "今日は晴れ。\nThe quick brown fox.\n".repeat(50);
        let comp = zstd_compress(original.as_bytes()).expect("メモリ zstd 圧縮は失敗しない");
        let back = zstd_decompress(&comp).unwrap();
        assert_eq!(back, original.as_bytes());
    }

    #[test]
    fn 壊れた_zstd_は_none() {
        assert!(zstd_decompress(&[0xff, 0x00, 0x12, 0x34]).is_none());
    }

    #[test]
    fn 退避種別の安定文字列() {
        assert_eq!(StashKind::BaselineReplace.as_str(), "baseline-replace");
        assert_eq!(StashKind::Incoming.as_str(), "incoming");
    }
}
