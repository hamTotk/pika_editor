// app/pipe_server: 名前付きパイプの実 I/O（単一インスタンスの原子的ロックと引数転送）。
// 要件3.2 / design.md 5.1 手順2「CreateNamedPipe を原子的ロックに・敗者はクライアント転送・
// サーバー公開はウィンドウ表示前」/ spec.md sprint3 must。
//
// 役割決定・転送 JSON 組み立て・パイプ名生成・SDDL/フラグ方針は wx/Win32 非依存の controller/core
// （decide_instance・make_pipe_name・make_owner_only_sddl・default_policy・serialize_request/
// parse_request）に置き、本モジュールはそれらの決定値を実 Win32（CreateNamedPipe/ConnectNamedPipe/
// ReadFile/WriteFile）へ流す薄い橋渡しに徹する。実 OS/FS 依存のためユニットテスト不能（系統B/C）。
//
// 原子性: CreateNamedPipe(FILE_FLAG_FIRST_PIPE_INSTANCE) は「最初の 1 プロセスだけが作成成功」する
// OS 保証を持つ。これをシングルインスタンスのロックに使い、TOCTOU を避ける（design.md 85行）。
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <string>

namespace pika::app
{

// 自プロセスのユーザー SID（"S-1-5-..."）を取得する。失敗時は空文字。
// パイプ名・SDDL の SID 部に使う（make_pipe_name/make_owner_only_sddl へ渡す）。
std::string current_user_sid();

// try_acquire の結果（3 値）。呼び出し側（main_gui）が役割決定へ橋渡しする。
enum class AcquireResult
{
    // FILE_FLAG_FIRST_PIPE_INSTANCE で最初に作成成功＝このプロセスがサーバー。
    Server,
    // 作成失敗（既存インスタンスあり）＝このプロセスはクライアント（引数を転送する）。
    Client,
    // owner-only DACL の SECURITY_DESCRIPTOR を構築できず、既定 DACL へフォールバックせずに
    // パイプを未作成にした（fail-closed）。owner-less パイプは絶対に公開しない。呼び出し側は
    // これをスタンドアロン縮退（IPC を張らず主インスタンスとして開く）として扱うこと。
    InsecureNotCreated,
};

// サーバー側: 受信した 1 行 JSON（信頼境界）を受け取るコールバック。
// 呼び出しは内部のリスナースレッドから来るため、実装は UI スレッドへマーシャリングすること
// （MainFrame 側は CallAfter で UI スレッドへ渡す）。
using OnRequestLine = std::function<void(const std::string& line)>;

// 単一インスタンスのパイプサーバー。CreateNamedPipe による原子的ロックを獲得し、
// 獲得できたら（サーバー）受信リスナースレッドを起動する。獲得できなければ acquired()=false。
class PipeServer
{
  public:
    PipeServer() = default;
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // pipe_name のパイプを FILE_FLAG_FIRST_PIPE_INSTANCE で作成しロックを試みる（owner-only DACL・
    // PIPE_REJECT_REMOTE_CLIENTS）。
    //  - 作成成功 → AcquireResult::Server（このプロセスがサーバー。acquired()=true）。
    //  - 作成失敗（既存あり） → AcquireResult::Client（このプロセスはクライアント）。
    //  - owner-only DACL の SECURITY_DESCRIPTOR を構築できない → 既定 DACL へフォールバックせず
    //    パイプを作らず AcquireResult::InsecureNotCreated を返す（fail-closed。owner-less パイプを
    //    公開しない。要件3.2）。呼び出し側はスタンドアロン縮退として扱う。
    // サーバーになれた場合のみ on_line を保持できる（公開＝start_listening）。
    AcquireResult try_acquire(const std::string& pipe_name, const std::string& owner_sddl);

    // ロックを獲得できたか（サーバーか）。
    bool acquired() const noexcept { return acquired_; }

    // 受信リスナースレッドを起動する（サーバーのときのみ有効）。design.md 5.1 手順2 に従い
    // ウィンドウ表示の前に呼ぶ（サーバー公開を表示前に完了させ TOCTOU を回避する）。
    void start_listening(OnRequestLine on_line);

    // リスナーを停止しパイプを閉じる（デストラクタからも呼ばれる）。
    void stop();

  private:
    void listen_loop();

    HANDLE pipe_ = INVALID_HANDLE_VALUE; // 獲得済みパイプ（サーバーのときのみ有効）
    HANDLE stop_event_ = nullptr;        // 停止合図（OVERLAPPED 待ちのキャンセル用）
    HANDLE thread_ = nullptr;            // リスナースレッド
    std::string pipe_name_;
    std::string owner_sddl_;
    OnRequestLine on_line_;
    bool acquired_ = false;
    bool listening_ = false;
};

// クライアント側: pipe_name のサーバーへ 1 行 JSON（transfer_json）を送る。
// 成功すれば true（敗者は終了コード0で終了する。design.md 5.1 手順2）。
// サーバーがまだ受信ループに入っていない瞬間は ERROR_PIPE_BUSY になりうるため短時間リトライする。
bool send_to_server(const std::string& pipe_name, const std::string& transfer_json);

} // namespace pika::app
