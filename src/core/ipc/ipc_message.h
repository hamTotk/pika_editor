// core/ipc: 引数転送メッセージのスキーマ（JSON 1行・最大数KB）。
// 要件3.2「受信は最大数KBで打ち切り、JSONスキーマ検証に失敗したデータは破棄してログに残す。
// 受理する操作はパス引数のオープンに限定する」/ design.md 95行「JSON 1行・最大数KB・スキーマ検証・
// 不正は破棄」。
//
// 信頼境界（受信側）の検証本体をここに置く（純ロジック・gtest 検証）。実際のパイプ I/O は
// プラットフォーム層が行い、受信した 1 行をそのまま parse_request に渡す。スキーマに
// 合致しない入力は破棄（parse は失敗を返す）し、受理する操作は open（パス列＋任意の
// -g 位置）に限定する。
#pragma once

#include <string>
#include <vector>

namespace pika::core::ipc
{

// 受信メッセージの最大バイト数（要件3.2「最大数KB」）。これを超える入力は読まずに破棄する。
inline constexpr std::size_t kMaxMessageBytes = 8 * 1024;

// 転送される 1 件の開く対象。path は絶対パス（クライアント側で正規化済み・要件3.2）。
// line/column は -g 由来（0 は「指定なし」）。
struct OpenTarget
{
    std::string path;
    int line = 0;   // 1 始まり。0 は指定なし
    int column = 0; // 1 始まり。0 は指定なし
};

// 受理する唯一の操作: パス引数のオープン。op は固定で "open"。
struct IpcRequest
{
    std::vector<OpenTarget> targets;
    // -g 由来でソースモード固定の意図か（要件3.1）。スキーマ上の任意フィールド。
    bool goto_mode = false;
};

// IpcRequest を 1 行 JSON（改行を含まない）にシリアライズする。クライアントが転送前に呼ぶ。
std::string serialize_request(const IpcRequest& req);

// 受信した 1 行 JSON を検証してリクエスト化する。要件3.2 の信頼境界:
//   - kMaxMessageBytes 超過は破棄（false）
//   - JSON として壊れていれば破棄（false）
//   - スキーマ不一致（op != "open"・targets が配列でない・path 欠落・絶対パスでない・
//     行/桁が整数でない等）は破棄（false）
//   - path が空・相対パス（先頭がドライブレターでも UNC でもない）は破棄（サーバー側 CWD 非依存）
// 成功時 true を返し out を埋める。失敗時 false（out は不定）。例外は投げない。
bool parse_request(const std::string& line, IpcRequest& out);

// 絶対パス判定（受信側スキーマ検証で使う・テストからも参照する）。
//   - `C:\...` / `C:/...`（ドライブレター＋コロン＋区切り）
//   - `\\server\share`（UNC）
// それ以外（相対・ドライブ相対 `C:foo`・空）は false。
bool is_absolute_windows_path(const std::string& path);

} // namespace pika::core::ipc
