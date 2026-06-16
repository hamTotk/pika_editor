// controller/app_controller: 起動オーケストレーションの wx 非依存ロジック。
// design.md 5.1（起動手順）・5.6（フォルダ切替）/ 要件3章（CLI・単一インスタンス・データルート）/
// spec.md sprint2 must。
//
// プラットフォーム層（main_gui）が集めた実環境入力（argv・cwd・FS 実在判定・自プロセス SID・
// パイプ獲得成否）を受け取り、(1)CLI 解析結果→開くべき OpenTarget
// 列への正規化、(2)単一インスタンスの 役割（サーバー/クライアント）決定と転送 JSON
// 組み立て、(3)起動手順の順序、(4)フォルダ切替の 状態遷移を決定論的に計算する。実
// I/O（パイプ作成・ウィンドウ生成・FS）は呼び出し側に委ね、ここは 純ロジックに保つ（gtest 検証）。
#pragma once

#include "core/ipc/cli_parser.h"
#include "core/ipc/ipc_message.h"

#include <string>
#include <vector>

namespace pika::controller
{

// CLI 受領の入力（プラットフォーム層が argv とプロセス環境から集める）。
struct CliContext
{
    // プログラム名を除いた引数列（main_gui が argv[1..] を UTF-8 化して渡す）。
    std::vector<std::string> args;
    // 呼び出し元プロセスのカレントディレクトリ（絶対パス）。相対パスの絶対化基準（要件3.2）。
    std::string cwd;
};

// 開くべき対象を正規化した結果。file_targets は絶対パス＋カーソル位置に確定済み。
// accepted=false なら GUI を起動せず exit_code で終了する（不正引数）。
// folder は開くワークスペースフォルダ（ExistingFolder。単一フォルダ方針）。空なら前回状態復元。
struct OpenPlan
{
    bool accepted = false;
    int exit_code = 0; // core/ipc の ExitCode と同値（0=受理 / 非0=不正引数）
    bool goto_mode = false;
    std::string folder;                              // 開くワークスペース（空＝指定なし）
    std::vector<core::ipc::OpenTarget> file_targets; // タブで開くファイル（絶対パス・カーソル位置）
    bool restore_previous = false;                   // 引数なし＝前回状態を復元する受理
    std::string message;                             // 失敗理由（診断ログ向け・内容は書かない）
};

// CLI を解析・検証し、開くべき対象を絶対パスへ正規化した OpenPlan を返す純粋関数。
// 手順（design.md 5.1 手順1 と一致）:
//   parse_argv → （help/version は GUI 非起動の受理として返す）→ validate（実在分類）→
//   各引数を cwd 基準で絶対パス化（normalize_to_absolute）→ ファイル/フォルダへ振り分け。
// probe は実在判定（FS アクセスをコアから排除）。help/version 指定は accepted=true・restore で
// なく targets 空で返す（実際の出力はコンソールスタブが担う。本フェーズの GUI は受け取らない）。
OpenPlan plan_open(const CliContext& ctx, const core::ipc::PathProbe& probe);

// 単一インスタンスの役割（design.md 5.1 手順2）。
enum class InstanceRole
{
    // CreateNamedPipe を獲得した唯一のプロセス。サーバー兼ウィンドウになる。
    Server,
    // 獲得に失敗（既存インスタンスあり）。クライアントとして引数を転送し終了コード0で終了する。
    Client,
};

// 単一インスタンス判定の入力。pipe_acquired は CreateNamedPipe の原子的ロック獲得成否（呼び出し側が
// 実 Win32 で確定して渡す。実パイプ I/O は sprint3 で配線）。user_sid は自プロセスのユーザー SID。
struct InstanceContext
{
    bool pipe_acquired = false;
    std::string user_sid;
    // セキュアな単一インスタンス保護（per-user パイプ名＋作成者のみ許可する
    // owner-only DACL）を構築できたか。false の条件＝
    //  (1) 自プロセス SID を取得できない（OpenProcessToken/GetTokenInformation
    //      失敗・制限トークン）→ per-user パイプ名も owner-only SDDL も作れない
    //  (2) owner-only DACL の SECURITY_DESCRIPTOR を構築できずパイプを未作成にした
    //      （pipe_server 第2層の fail-closed）
    // いずれも owner-less パイプを公開してはならず、ユーザー別分離も失われる。
    // このとき IPC を一切張らずスタンドアロン起動（主インスタンスとして開く・
    // 転送しない）へ縮退する必要があるため、decide_instance はこのフラグを最優先で
    // 見る（要件3.2 の fail-closed・要件12.3）。
    bool secure_isolation_available = true;
};

// 単一インスタンス判定の結果。role がクライアントのとき、転送すべきパイプ名と 1 行 JSON が埋まる。
// サーバーのときは pipe_name のみ（自分が公開すべきパイプ名）。
struct InstanceDecision
{
    InstanceRole role = InstanceRole::Server;
    std::string pipe_name;     // \\.\pipe\pika-<SID>（make_pipe_name）
    std::string transfer_json; // クライアント時のみ非空（serialize_request の結果・1 行）
    int client_exit_code =
        0; // クライアントの終了コード（受理＝0。design.md 5.1 手順2「敗者は 0」）
};

// 単一インスタンスの役割を決め、クライアントなら転送 JSON を組み立てる純粋関数。
// - secure_isolation_available=false → Server（スタンドアロン縮退。pipe_acquired は無視し、
//   転送しない＝transfer_json は空）。セキュアなパイプを作れない以上、敗者クライアントとして
//   存在しない/owner-less なサーバーへ転送しに行く経路には絶対に落とさない（fail-closed）。
// - secure_isolation_available=true かつ pipe_acquired=true → Server（pipe_name のみ）。
// - secure_isolation_available=true かつ pipe_acquired=false →
//   Client（pipe_name ＋ plan を IpcRequest 化した transfer_json）。
//   転送するのは plan の絶対パス化済み対象のみ（サーバー CWD 非依存。要件3.2）。
InstanceDecision decide_instance(const InstanceContext& inst, const OpenPlan& plan);

// 起動手順のステップ列（design.md 5.1）。順序が正典と一致することをテストで観測する。
enum class StartupStep
{
    ResolveDataRoot, // 1: portable.txt 検出 → データルート確定
    ParseCli,        // 1: -g パース・引数検証
    DecideInstance,  // 2: 単一インスタンス判定（サーバー/クライアント）
    ShowWindow,      // 3: MainFrame 生成・表示（最短経路）
    AsyncLoad,       // 4: ツリー列挙・監視開始・未読判定・状態復元（表示後）
};

// design.md 5.1 の起動手順を順序付きステップ列として返す（順序の正典との一致を観測するため）。
std::vector<StartupStep> startup_sequence();

} // namespace pika::controller
