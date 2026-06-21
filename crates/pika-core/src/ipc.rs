//! 単一インスタンスの自前 named pipe プロトコル（要件3.2・design doc 9章/15章-9）。
//!
//! design doc 15章-9: 単一インスタンスは**自前 named pipe を既定**とする。
//! - パイプ名は `\\.\pipe\pika-<ユーザーSID>`（ユーザーごとに分離）。
//! - ユーザー限定 DACL・`PIPE_REJECT_REMOTE_CLIENTS`・受信 ≤ 数 KB 打切り。
//! - 受理操作は**パスオープン限定**（任意 command を受け付けない＝信頼境界を狭く保つ）。
//! - `CreateNamedPipe` の成否を原子的ロックとし、獲得失敗プロセスはクライアントとして
//!   絶対パス正規化済み引数を転送し終了コード0で終了する。
//!
//! 本モジュールは I/O を行わない純粋ロジック（cargo test の決定論ゲート対象）。
//! 実 OS 呼び出し（`CreateNamedPipe`/`CreateFile`/DACL 設定/spawn）は `src-tauri`/`pika-cli` が行い、
//! ここには「パイプ名組立・転送 JSON 組立/スキーマ検証・受理操作の決定論モデル」を集約する。

use crate::error::{PikaError, Result};
use crate::path_verify::verify_received_path;
use serde::{Deserialize, Serialize};

/// 転送 JSON の打切り上限（受信 ≤ 数 KB＝design doc 15章-9）。これを超える受信は破棄する。
pub const MAX_MESSAGE_BYTES: usize = 8 * 1024;

/// プロトコルのバージョン。互換性のない受信メッセージは安全側に倒して拒否する。
pub const PROTOCOL_VERSION: u32 = 1;

/// パイプ名のホスト部固定接頭辞（`\\.\pipe\` はローカル名前空間）。
const PIPE_PREFIX: &str = r"\\.\pipe\pika-";

/// 単一インスタンスで転送する 1 メッセージ（受理操作=パスオープン限定）。
///
/// 任意 command を受け付けないため、フィールドは「開くパス」と「-g カーソル位置」のみ。
/// 後方互換のため未知フィールドは無視する（serde 既定）。version は安全側判定に使う。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct OpenRequest {
    /// プロトコルバージョン（不一致は拒否＝design doc 9章「version 安全側」）。
    pub version: u32,
    /// 開くパス群（**絶対パス正規化済み**でクライアントが入れる規約。サーバーは再検証する）。
    pub paths: Vec<String>,
    /// `-g` カーソル位置（任意）。`<path>` は paths の先頭に対応する。
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub goto: Option<GotoPosition>,
}

/// 転送する `-g` のカーソル位置（1 始まり）。
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GotoPosition {
    pub line: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub column: Option<u32>,
}

/// このプロセスが担う役割（`CreateNamedPipe` の成否で決まる＝原子的ロック）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum InstanceRole {
    /// パイプ獲得に成功＝**最初の**インスタンス。サーバーとして常駐し GUI を起動する。
    Server,
    /// パイプ獲得に失敗＝既に起動済み。クライアントとして引数を転送し終了コード0で終了する。
    Client,
}

/// `CreateNamedPipe` の成否（原子的ロック）から役割を決定する純粋関数。
///
/// 成功（`pipe_created == true`）＝サーバー、失敗（既存サーバーあり）＝クライアント。
/// この判定は OS の名前付きオブジェクト一意性に依拠するため、競合は OS が直列化する
/// （design doc 9章「`CreateNamedPipe` の成否を原子的ロックとする」）。
pub fn decide_role(pipe_created: bool) -> InstanceRole {
    if pipe_created {
        InstanceRole::Server
    } else {
        InstanceRole::Client
    }
}

/// パイプ名を組み立てる（`\\.\pipe\pika-<SID>`）。
///
/// SID をユーザーごとの分離キーにする（design doc 15章-9）。SID は呼び出し側が
/// `GetTokenInformation`/`ConvertSidToStringSid` で取得して渡す（ここでは文字列組立のみ）。
/// SID 文字列が空・不正（パイプ名に使えない文字）なら拒否する（注入防止）。
pub fn build_pipe_name(user_sid: &str) -> Result<String> {
    if user_sid.is_empty() {
        return Err(PikaError::InvalidArgument(
            "パイプ名の SID が空".into(),
        ));
    }
    // SID 文字列は `S-1-5-...` 形式（英数字とハイフンのみ）。それ以外はパイプ名注入を防ぐため拒否。
    if !user_sid
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || c == '-')
    {
        return Err(PikaError::InvalidArgument(format!(
            "SID にパイプ名へ使えない文字が含まれる: {user_sid}"
        )));
    }
    Ok(format!("{PIPE_PREFIX}{user_sid}"))
}

/// クライアントが転送する `OpenRequest` の JSON を組み立てる（絶対パス正規化済み引数から）。
///
/// `paths` は呼び出し側で絶対パス化済みである規約だが、念のためここでも空要素を除く。
/// version は固定値を入れる（受信側が version 検証で互換性を判断する）。
pub fn build_forward_message(paths: &[String], goto: Option<GotoPosition>) -> Result<String> {
    let paths: Vec<String> = paths
        .iter()
        .filter(|p| !p.trim().is_empty())
        .cloned()
        .collect();
    let req = OpenRequest {
        version: PROTOCOL_VERSION,
        paths,
        goto,
    };
    serde_json::to_string(&req)
        .map_err(|e| PikaError::InvalidArgument(format!("転送 JSON の組立に失敗: {e}")))
}

/// サーバーが受信した生バイト列を検証し、受理可能な `OpenRequest` を返す（受理操作=パスオープン限定）。
///
/// 検証段（信頼境界＝design doc 9章）:
/// 1. 受信長 ≤ `MAX_MESSAGE_BYTES`（打切り。巨大入力を弾く）。
/// 2. JSON スキーマ検証（`OpenRequest` へデシリアライズできること）。
/// 3. version 一致（不一致は安全側に倒して拒否）。
/// 4. 各パスを `path_verify` で再正規化・再検証（転送パスを信頼しない）。
/// 5. 受理操作はパスオープンのみ＝任意 command フィールドは型に存在しないので受け付けようがない。
pub fn parse_incoming_message(raw: &[u8]) -> Result<OpenRequest> {
    if raw.len() > MAX_MESSAGE_BYTES {
        return Err(PikaError::InvalidArgument(format!(
            "受信メッセージが上限 {MAX_MESSAGE_BYTES} バイトを超過（打切り）"
        )));
    }
    let text = std::str::from_utf8(raw)
        .map_err(|_| PikaError::InvalidArgument("受信メッセージが UTF-8 でない".into()))?;
    let mut req: OpenRequest = serde_json::from_str(text)
        .map_err(|e| PikaError::InvalidArgument(format!("受信メッセージのスキーマ不正: {e}")))?;

    // version 安全側: 既知バージョンのみ受理する（未知は読まず拒否＝design doc 9章）。
    if req.version != PROTOCOL_VERSION {
        return Err(PikaError::InvalidArgument(format!(
            "未知のプロトコルバージョン {} を拒否（既知は {PROTOCOL_VERSION}）",
            req.version
        )));
    }

    // 受信パスを 1 件ずつ再検証し、正規化済みパスへ置き換える（転送パスを信頼しない）。
    let mut verified = Vec::with_capacity(req.paths.len());
    for p in &req.paths {
        let v = verify_received_path(p)?;
        verified.push(v.normalized);
    }
    req.paths = verified;
    Ok(req)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn パイプ獲得成功はサーバー役割() {
        assert_eq!(decide_role(true), InstanceRole::Server);
    }

    #[test]
    fn パイプ獲得失敗はクライアント役割() {
        assert_eq!(decide_role(false), InstanceRole::Client);
    }

    #[test]
    fn パイプ名は_sid_を含む() {
        let name = build_pipe_name("S-1-5-21-1004336348-1177238915-682003330-1001").unwrap();
        assert!(name.starts_with(r"\\.\pipe\pika-"));
        assert!(name.ends_with("S-1-5-21-1004336348-1177238915-682003330-1001"));
    }

    #[test]
    fn 空_sid_は拒否する() {
        assert!(build_pipe_name("").is_err());
    }

    #[test]
    fn 注入を含む_sid_は拒否する() {
        // パイプ名へ別の名前空間を仕込む注入を弾く。
        assert!(build_pipe_name(r"S-1-5\..\evil").is_err());
        assert!(build_pipe_name("S-1-5 evil").is_err());
    }

    #[test]
    fn 転送_json_を組み立てて往復できる() {
        let json = build_forward_message(
            &[r"C:\a.md".to_string()],
            Some(GotoPosition {
                line: 12,
                column: Some(3),
            }),
        )
        .unwrap();
        let req = parse_incoming_message(json.as_bytes()).unwrap();
        assert_eq!(req.paths, vec![r"C:\a.md".to_string()]);
        assert_eq!(req.goto.as_ref().unwrap().line, 12);
        assert_eq!(req.goto.as_ref().unwrap().column, Some(3));
    }

    #[test]
    fn 転送時に空パスを除去する() {
        let json =
            build_forward_message(&["".to_string(), "  ".to_string(), r"C:\a.md".to_string()], None)
                .unwrap();
        let req = parse_incoming_message(json.as_bytes()).unwrap();
        assert_eq!(req.paths, vec![r"C:\a.md".to_string()]);
    }

    #[test]
    fn 上限超過の受信は打切り拒否() {
        let big = vec![b'x'; MAX_MESSAGE_BYTES + 1];
        assert!(parse_incoming_message(&big).is_err());
    }

    #[test]
    fn 非_json_受信はスキーマ不正で拒否() {
        assert!(parse_incoming_message(b"not json at all").is_err());
    }

    #[test]
    fn 未知バージョンは安全側に倒して拒否() {
        let raw = r#"{"version":999,"paths":["C:\\a.md"]}"#;
        assert!(parse_incoming_message(raw.as_bytes()).is_err());
    }

    #[test]
    fn 受信パスは再検証され相対パスは拒否される() {
        // クライアントが（規約違反で）相対パスを送っても、サーバーが再検証で弾く。
        let raw = r#"{"version":1,"paths":["relative\\a.md"]}"#;
        assert!(parse_incoming_message(raw.as_bytes()).is_err());
    }

    #[test]
    fn 受信パスの_ads_参照は再検証で拒否される() {
        let raw = r#"{"version":1,"paths":["C:\\a.md:stream"]}"#;
        assert!(parse_incoming_message(raw.as_bytes()).is_err());
    }

    #[test]
    fn 受信メッセージは余分なフィールドを無視する() {
        // 受理操作=パスオープン限定。任意 command を載せても OpenRequest には吸われない。
        let raw = r#"{"version":1,"paths":["C:\\a.md"],"command":"rm -rf","exec":true}"#;
        let req = parse_incoming_message(raw.as_bytes()).unwrap();
        assert_eq!(req.paths, vec![r"C:\a.md".to_string()]);
    }

    #[test]
    fn goto_省略時は_none() {
        let json = build_forward_message(&[r"C:\a.md".to_string()], None).unwrap();
        let req = parse_incoming_message(json.as_bytes()).unwrap();
        assert!(req.goto.is_none());
    }
}
