//! watcher 合成層の入出力型（design doc 4章 `FsEvent`・要件7.1/4.2）。
//!
//! `RawFsEvent` は監視スレッドが OS 由来イベントから写す**抽象**入力（notify 型を漏らさない）。
//! `FsChange` は本層がデバウンス/正規化した後にフロントへ送る**合成結果**。

/// ファイルの同一性を OS のファイル ID で補強するための識別子（rename 正規化に使う）。
///
/// Windows では `nFileIndexHigh/Low`＋ボリュームシリアル相当、Unix では `dev`＋`ino` を
/// 監視スレッド側で取得して詰める。取得できない FS（一部ネットワーク/クラウド）では `None`。
/// パス名だけでは相互スワップ（A↔B）や上書き rename を取りこぼすため、ID が取れる時は ID を優先する。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct FileId {
    /// ボリューム/デバイスのシリアル（Windows: volume serial / Unix: st_dev）。
    pub volume: u64,
    /// ファイルのインデックス（Windows: file index / Unix: st_ino）。
    pub index: u64,
}

/// 監視スレッドが OS 由来イベントから写す抽象 raw event 種別。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RawFsEventKind {
    /// 新規作成。
    Created,
    /// 内容変更（書き込み・属性変更も含めて変更候補として扱う）。
    Modified,
    /// 削除。
    Removed,
    /// rename の旧名側（移動元）。
    RenamedFrom,
    /// rename の新名側（移動先）。
    RenamedTo,
    /// 監視バッファのオーバーフロー（ERROR_NOTIFY_ENUM_DIR 相当）。
    /// これを受けたら該当監視ルートを全再列挙して再同期する（[`overflow`]）。
    Overflow,
}

/// 監視スレッドが本層へ流し込む抽象 raw event。
///
/// I/O を行わないため、確定読みに必要な軽量メタ（mtime/サイズ/FileId）は監視スレッドが
/// 添えて渡す（本層は FS を触らない＝決定論を保つ）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawFsEvent {
    /// 種別。
    pub kind: RawFsEventKind,
    /// 対象パス（オーバーフローでは監視ルート、ペアの片側ではそのパス）。
    pub path: String,
    /// 監視スレッドが採取したイベント発生時刻（単調増加のミリ秒。テストでは論理時刻）。
    pub at_ms: u64,
    /// FileId（取得できた場合のみ。rename 正規化の主キー）。
    pub file_id: Option<FileId>,
    /// 確定読み判定に使う mtime（ミリ秒。取得不可なら `None`）。
    pub mtime_ms: Option<u64>,
    /// 確定読み判定に使うサイズ（バイト。取得不可なら `None`）。
    pub size: Option<u64>,
}

/// 本層が合成し終えてフロントへ送る変更（要件4.2 の未読規則に対応）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FsChangeKind {
    /// 新規作成（未読 ◆）。
    Created,
    /// 内容変更（未読 ±）。
    Modified,
    /// 削除（取り消し線）。
    Removed,
    /// rename/移動（未読・ベースライン・退避を旧→新へ引き継ぐ。要件4.2）。
    Renamed {
        /// 旧パス。
        from: String,
    },
}

/// 合成済みの 1 変更。フロントの `emit('fs-changed')` のペイロード単位。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FsChange {
    /// 種別。
    pub kind: FsChangeKind,
    /// 対象パス（rename では新パス）。
    pub path: String,
}

impl FsChange {
    /// 新規作成の合成結果を作る。
    pub fn created(path: impl Into<String>) -> Self {
        Self {
            kind: FsChangeKind::Created,
            path: path.into(),
        }
    }

    /// 内容変更の合成結果を作る。
    pub fn modified(path: impl Into<String>) -> Self {
        Self {
            kind: FsChangeKind::Modified,
            path: path.into(),
        }
    }

    /// 削除の合成結果を作る。
    pub fn removed(path: impl Into<String>) -> Self {
        Self {
            kind: FsChangeKind::Removed,
            path: path.into(),
        }
    }

    /// rename の合成結果を作る（`from`=旧パス・`path`=新パス）。
    pub fn renamed(from: impl Into<String>, to: impl Into<String>) -> Self {
        Self {
            kind: FsChangeKind::Renamed { from: from.into() },
            path: to.into(),
        }
    }
}
