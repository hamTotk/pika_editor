// app/perf_log: 系統C A章（性能ゲート）の in-app 計測 instrumentation（F-026）。
// docs/acceptance.md A章・design.md 11章「性能設計（予算の分解）」/ acceptance-findings.md F-026。
//
// 目的（F-026 の2つのブロッカーを in-app で解く）:
//   (1) 「ウィンドウ表示完了」「再表示完了」等の瞬間を外部から正確に取れない
//       → QueryPerformanceCounter で各マイルストーンをアプリ内部で直接スタンプし区間 ms
//       を算出する。
//   (2) WebView2 が別プロセス（msedgewebview2.exe）で動き、本環境では他アプリの WebView2 と混在し
//       pika 配下だけを分離計測できない
//       → GetProcessMemoryInfo で自プロセス、CreateToolhelp32Snapshot で自分の子孫プロセス
//         （msedgewebview2.exe 等）を再帰的に列挙して WorkingSetSize を合算し、pika プロセスツリー
//         全体のメモリを出す（他アプリの WebView2 と混ざらない）。
//
// 原則③「軽い」: 計測は既定オフ。有効時（--perf-log）のみログファイルを書く。常時の QPC
// スタンプ自体は 安価なので残してよいが、ファイル I/O は有効時だけ。レイヤー:
// Win32（QPC/psapi/Toolhelp32）はこの プラットフォーム層に閉じる（core/ に Win32
// を持ち込まない）。UI 層は mark()/measure()/memory_*() を 呼ぶだけで Win32 を知らない。
//
// プライバシー（要件12.3）: ログにユーザーのファイル内容・パスは一切書かない。マイルストーン名・
// 区間 ms・基準値・PASS/FAIL・メモリ（自分/ツリー合算）のみを書く。
#pragma once

#include <cstdint>
#include <string>

namespace pika::app
{

// 計測マイルストーン（A1〜A8）。区間の起点/終点をこの識別子で対応づける。
// タイミング系（A1〜A6）は begin→end の 2 点、メモリ系（A7/A8）は 1 点で記録する。
enum class PerfMark
{
    // A1: プロセス開始（OnInit 入口）→ MainFrame が実際に画面へ出た瞬間（Show 後の最初の
    // idle/paint）。
    StartupBegin,
    StartupShown,
    // A2: TrySuspend 実行 → Resume（再ナビゲートでの再表示完了）。
    SuspendDone,
    ResumeShown,
    // A3: プレビュー初回ナビゲート開始 → on_loaded 完了。
    PreviewFirstNavBegin,
    PreviewFirstLoaded,
    // A4: タブ/モード切替トリガ → 再描画（on_loaded）完了（2 回目以降のキャッシュ復元経路）。
    SwitchBegin,
    SwitchLoaded,
    // A5: 編集イベント → デバウンス後のプレビュー更新（on_loaded）完了。
    EditBegin,
    EditLoaded,
    // A6: 外部変更検知 → エディタ/ツリー反映完了。
    ExternalChangeBegin,
    ExternalChangeApplied,
    // A7: 既定プレビュー直後メモリ（1 点記録）。
    MemoryAfterPreview,
    // A8: プレビュー後アイドルメモリ（TrySuspend で WS 縮小。1 点記録）。
    MemoryIdleAfterSuspend,
};

// プロセスツリーのメモリ計測結果（WorkingSetSize ベース・バイト）。
struct MemorySnapshot
{
    std::uint64_t self_ws_bytes = 0; // 自プロセスの WorkingSetSize
    std::uint64_t tree_ws_bytes = 0; // 自プロセス＋全子孫（WebView2 等）の WorkingSetSize 合算
    std::uint32_t process_count = 0; // 合算に含めたプロセス数（自身を含む）
};

// 系統C 性能計測のファサード（プロセス内シングルトン）。UI/プラットフォーム各所の計測点から呼ぶ。
// 既定は無効（enabled()==false）で、mark/measure/memory_* は QPC スタンプ等の安価な処理のみ行い
// ファイルへは書かない。enable() 後に限ってログ行を追記する。
class PerfLog
{
  public:
    // プロセス内の唯一のインスタンス。
    static PerfLog& instance();

    // 計測を有効化する（--perf-log 指定時に main_gui が 1 回呼ぶ）。log_path はログ出力先
    // （空なら %TEMP%\pika-perf.log）。既に有効でも冪等（再オープンしない）。
    void enable(const std::string& log_path = {});

    // 計測が有効か（有効時のみファイル書き込みを行う）。
    bool enabled() const noexcept { return enabled_; }

    // 起点（begin 系マイルストーン）の QPC をスタンプする。end 側 measure() で区間 ms を出す。
    // 無効時も安価に記録する（ファイルには書かない）。
    void mark(PerfMark begin);

    // 起点 begin から終点 end までの区間 ms を算出し、有効時はログへ 1 行書く。
    // budget_ms は受け入れ基準（0 なら基準なし）。target_ms は目標値（0 なら省略）。
    // capture_memory=true のとき、その行にプロセスツリーのメモリ（自分/合算）を併記する。
    // begin がまだ mark されていない場合は何もしない（区間が確定できない）。
    void measure(PerfMark begin, PerfMark end, double budget_ms, double target_ms = 0.0,
                 bool capture_memory = false);

    // メモリ単発記録（A7/A8）。プロセスツリーの WorkingSetSize を計測し、有効時はログへ 1 行書く。
    // budget_mb は受け入れ基準の目安（MB・0 なら基準なし）。戻り値は計測したスナップショット。
    MemorySnapshot record_memory(PerfMark mark, double budget_mb);

    // プロセスツリー（自プロセス＋全子孫）の WorkingSetSize
    // を計測する（ログを書かずに値だけ返す）。
    // Win32（psapi/Toolhelp32）に閉じた実体。失敗時は取得できた範囲を返す（0 を返さず縮退）。
    static MemorySnapshot capture_memory_snapshot();

  private:
    PerfLog() = default;

    // 有効時に 1 行追記する（UTF-8・wxFFile）。末尾改行付き。無効時は何もしない。
    void write_line(const std::string& line);

    bool enabled_ = false;
    std::string log_path_;
    // begin 系マイルストーンの QPC（end 側で区間算出に使う）。PerfMark の総数ぶん確保する。
    std::int64_t stamps_[16] = {};
    bool stamped_[16] = {};
    std::int64_t qpc_freq_ = 0; // QueryPerformanceFrequency（ticks/sec・0 なら未初期化）
};

} // namespace pika::app
