// app/watch_thread: ReadDirectoryChangesW を回す監視スレッド実体（プラットフォーム層）。
// design.md 5.2（外部変更の監視・バッファオーバーフロー回復・監視不能環境のフォールバック）/
// 要件7章・11.2（F5）/ spec.md sprint4 must（監視スレッド実体・ポーリング・オーバーフロー再同期）。
//
// 監視ルートを ReadDirectoryChangesW（OVERLAPPED）で監視し、届く FILE_NOTIFY_INFORMATION を
// 1 件ずつ controller::make_raw_event で RawEvent（'/'区切り UTF-8）へ正規化し、UI スレッドへ
// on_raw コールバックで渡す（重い処理はスレッドでしない。design.md 4章「合成して積むだけ」）。
// WatcherCore への投入・確定読み・再同期は UI スレッドが担う（WatcherCore は非スレッドセーフ）。
//
// 監視不能環境（UNC・一部クラウド）では ReadDirectoryChangesW が開始できないため、定期ポーリング
// （既定5秒）の合図を on_resync で UI スレッドへ送る。バッファオーバーフロー
// （ERROR_NOTIFY_ENUM_DIR）でも on_resync を送り、UI 側が全再列挙→resync で取りこぼしを回復する。
// F5（要件11.2）は同じ resync をオンデマンド実行する（request_resync）。実 OS/FS 依存＝系統B/C。
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/watcher/fs_event.h"

#include <functional>
#include <string>

namespace pika::app
{

// 確定前の生イベント 1 件を UI スレッドへ渡すコールバック（'/'区切り UTF-8 へ正規化済み）。
// リスナースレッドから呼ばれるため、実装は UI スレッドへマーシャリングすること（MainFrame は
// CallAfter で WatcherCore::on_raw へ渡す）。
using OnRawEvent = std::function<void(const core::watcher::RawEvent& ev)>;

// 再同期が必要になったことを UI スレッドへ知らせるコールバック（理由付き）。
//   - バッファオーバーフロー（取りこぼし）
//   - 監視不能環境の定期ポーリング tick
// UI 側は resync（全再列挙→突き合わせ）を実行し WorkspaceController へ反映する。
enum class ResyncReason
{
    Overflow, // ERROR_NOTIFY_ENUM_DIR（バッファ溢れ）。取りこぼし回復。
    Poll,     // 監視不能環境の定期ポーリング（既定5秒）。
    Manual,   // F5（要件11.2）。オンデマンド再同期。
};
using OnResyncNeeded = std::function<void(ResyncReason reason)>;

// 監視スレッド。start で監視ルートに対し監視 or ポーリングを開始する。
class WatchThread
{
  public:
    // 監視不能環境でのポーリング間隔の既定（ms。design.md 5.2「既定5秒・設定可」）。
    static constexpr DWORD kDefaultPollIntervalMs = 5000;

    WatchThread() = default;
    ~WatchThread();

    WatchThread(const WatchThread&) = delete;
    WatchThread& operator=(const WatchThread&) = delete;

    // root_abs（UTF-8 絶対パス）の監視を開始する。on_raw は生イベント、on_resync は再同期合図。
    // ReadDirectoryChangesW を開始できない環境では定期ポーリングへ自動フォールバックする
    // （poll_interval_ms ごとに on_resync(Poll) を送る）。
    void start(const std::string& root_abs, OnRawEvent on_raw, OnResyncNeeded on_resync,
               DWORD poll_interval_ms = kDefaultPollIntervalMs);

    // F5（要件11.2）相当。次の機会に on_resync(Manual) を 1 回送る（オンデマンド再同期）。
    void request_resync();

    // 監視/ポーリングを停止しスレッドを畳む（デストラクタからも呼ばれる）。
    void stop();

    // 監視中か（ReadDirectoryChangesW を回せているか。false＝ポーリングフォールバック中）。
    bool watching() const noexcept { return watching_; }

  private:
    static unsigned __stdcall thread_entry(void* arg); // _beginthreadex のエントリ
    void run();
    bool open_dir(const std::string& root_abs); // 監視ハンドルを開く（失敗＝ポーリング）

    HANDLE dir_ = INVALID_HANDLE_VALUE;
    HANDLE stop_event_ = nullptr;   // 停止合図
    HANDLE resync_event_ = nullptr; // F5（手動再同期）合図
    HANDLE thread_ = nullptr;
    std::string root_;
    OnRawEvent on_raw_;
    OnResyncNeeded on_resync_;
    DWORD poll_interval_ms_ = kDefaultPollIntervalMs;
    bool watching_ = false;
    bool started_ = false;
};

} // namespace pika::app
