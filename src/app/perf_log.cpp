#include "app/perf_log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/string.h>

#include <cstdio>
#include <vector>

namespace pika::app
{

namespace
{

// マイルストーン → ログ表示名（A 番号付き・固定 ASCII。ユーザー内容を含まない）。
const char* mark_label(PerfMark m)
{
    switch (m)
    {
    case PerfMark::StartupBegin:
        return "A1.startup_begin";
    case PerfMark::StartupShown:
        return "A1.startup_to_window";
    case PerfMark::SuspendDone:
        return "A2.suspend_done";
    case PerfMark::ResumeShown:
        return "A2.resume_to_shown";
    case PerfMark::PreviewFirstNavBegin:
        return "A3.preview_first_nav";
    case PerfMark::PreviewFirstLoaded:
        return "A3.preview_first_loaded";
    case PerfMark::SwitchBegin:
        return "A4.switch_begin";
    case PerfMark::SwitchLoaded:
        return "A4.switch_loaded";
    case PerfMark::EditBegin:
        return "A5.edit_begin";
    case PerfMark::EditLoaded:
        return "A5.edit_preview_loaded";
    case PerfMark::ExternalChangeBegin:
        return "A6.extchange_begin";
    case PerfMark::ExternalChangeApplied:
        return "A6.extchange_applied";
    case PerfMark::MemoryAfterPreview:
        return "A7.mem_after_preview";
    case PerfMark::MemoryIdleAfterSuspend:
        return "A8.mem_idle_after_suspend";
    }
    return "unknown";
}

// 終点（end 系）→ 区間ラベル（measure のログ行で使う）。begin 系には呼ばない。
const char* interval_label(PerfMark end)
{
    switch (end)
    {
    case PerfMark::StartupShown:
        return "A1.startup_to_window";
    case PerfMark::ResumeShown:
        return "A2.resume_to_shown";
    case PerfMark::PreviewFirstLoaded:
        return "A3.preview_first_show";
    case PerfMark::SwitchLoaded:
        return "A4.switch_redraw";
    case PerfMark::EditLoaded:
        return "A5.edit_preview_update";
    case PerfMark::ExternalChangeApplied:
        return "A6.extchange_reflect";
    default:
        return mark_label(end);
    }
}

std::int64_t qpc_now()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<std::int64_t>(c.QuadPart);
}

// PerfMark を stamps_ 配列の添字へ（enum 値そのまま・範囲外は -1）。
int mark_index(PerfMark m)
{
    const int i = static_cast<int>(m);
    if (i < 0 || i >= 16)
    {
        return -1;
    }
    return i;
}

double bytes_to_mb(std::uint64_t b)
{
    return static_cast<double>(b) / (1024.0 * 1024.0);
}

} // namespace

PerfLog& PerfLog::instance()
{
    static PerfLog inst;
    return inst;
}

void PerfLog::enable(const std::string& log_path)
{
    if (enabled_)
    {
        return; // 冪等（二重有効化でも再オープンしない）。
    }
    enabled_ = true;
    if (!log_path.empty())
    {
        log_path_ = log_path;
    }
    else
    {
        // 既定: %TEMP%\pika-perf.log（wxFileName::GetTempDir 経由）。ユーザーフォルダは汚さない。
        wxFileName fn(wxFileName::GetTempDir(), "pika-perf.log");
        log_path_ = std::string(fn.GetFullPath().ToUTF8().data());
    }
    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f))
    {
        qpc_freq_ = static_cast<std::int64_t>(f.QuadPart);
    }
    // 起動ごとの区切りを 1 行入れる（測定セッションの境目を読みやすくする）。
    write_line("==== pika perf session ====");
}

void PerfLog::mark(PerfMark begin)
{
    const int i = mark_index(begin);
    if (i < 0)
    {
        return;
    }
    if (qpc_freq_ == 0)
    {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f))
        {
            qpc_freq_ = static_cast<std::int64_t>(f.QuadPart);
        }
    }
    stamps_[i] = qpc_now();
    stamped_[i] = true;
}

void PerfLog::measure(PerfMark begin, PerfMark end, double budget_ms, double target_ms,
                      bool capture_memory)
{
    const int bi = mark_index(begin);
    if (bi < 0 || !stamped_[bi] || qpc_freq_ == 0)
    {
        return; // 起点未記録＝区間を確定できない（観測のみ・例外を投げない）。
    }
    const std::int64_t end_qpc = qpc_now();
    const double ms =
        static_cast<double>(end_qpc - stamps_[bi]) * 1000.0 / static_cast<double>(qpc_freq_);

    if (!enabled_)
    {
        return; // 無効時はファイルへ書かない（QPC スタンプの計算だけ・捨てる）。
    }

    const bool pass = (budget_ms <= 0.0) || (ms <= budget_ms);
    char buf[512];
    if (target_ms > 0.0)
    {
        std::snprintf(buf, sizeof(buf), "%-26s %8.1f ms  budget<=%.0f target<=%.0f  %s",
                      interval_label(end), ms, budget_ms, target_ms, pass ? "PASS" : "FAIL");
    }
    else if (budget_ms > 0.0)
    {
        std::snprintf(buf, sizeof(buf), "%-26s %8.1f ms  budget<=%.0f             %s",
                      interval_label(end), ms, budget_ms, pass ? "PASS" : "FAIL");
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%-26s %8.1f ms", interval_label(end), ms);
    }
    std::string line(buf);

    if (capture_memory)
    {
        const MemorySnapshot snap = capture_memory_snapshot();
        char mbuf[160];
        std::snprintf(mbuf, sizeof(mbuf), "  mem self=%.0fMB tree=%.0fMB(%u proc)",
                      bytes_to_mb(snap.self_ws_bytes), bytes_to_mb(snap.tree_ws_bytes),
                      snap.process_count);
        line += mbuf;
    }
    write_line(line);
}

MemorySnapshot PerfLog::record_memory(PerfMark mark, double budget_mb)
{
    const MemorySnapshot snap = capture_memory_snapshot();
    if (!enabled_)
    {
        return snap;
    }
    const double tree_mb = bytes_to_mb(snap.tree_ws_bytes);
    const bool pass = (budget_mb <= 0.0) || (tree_mb <= budget_mb);
    char buf[256];
    if (budget_mb > 0.0)
    {
        std::snprintf(buf, sizeof(buf),
                      "%-26s          mem self=%.0fMB tree=%.0fMB(%u proc)  "
                      "budget<=%.0fMB  %s",
                      mark_label(mark), bytes_to_mb(snap.self_ws_bytes), tree_mb,
                      snap.process_count, budget_mb, pass ? "PASS" : "FAIL");
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%-26s          mem self=%.0fMB tree=%.0fMB(%u proc)",
                      mark_label(mark), bytes_to_mb(snap.self_ws_bytes), tree_mb,
                      snap.process_count);
    }
    write_line(std::string(buf));
    return snap;
}

MemorySnapshot PerfLog::capture_memory_snapshot()
{
    MemorySnapshot out;

    // 1) 自プロセスの WorkingSetSize。
    const DWORD self_pid = ::GetCurrentProcessId();
    {
        PROCESS_MEMORY_COUNTERS pmc;
        ZeroMemory(&pmc, sizeof(pmc));
        pmc.cb = sizeof(pmc);
        if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            out.self_ws_bytes = static_cast<std::uint64_t>(pmc.WorkingSetSize);
        }
    }
    out.tree_ws_bytes = out.self_ws_bytes;
    out.process_count = 1;

    // 2) 全プロセスのスナップショットから親子関係を作り、自プロセスの子孫を BFS で集める。
    //    msedgewebview2.exe（WebView2）は子（さらに孫）として立つため、ParentProcessID を
    //    起点 PID から再帰的に辿って合算する＝他アプリの WebView2 と混ざらない（F-026）。
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        return out; // 取れた範囲（自プロセスのみ）で縮退して返す（0 を返さない）。
    }

    struct Proc
    {
        DWORD pid = 0;
        DWORD ppid = 0;
    };
    std::vector<Proc> all;
    all.reserve(256);
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe))
    {
        do
        {
            // PID 0/4（System Idle/System）は親として共有され得るため、自身を親とする偽の
            // 子孫を作らないよう除外する（th32ProcessID==0 の行は集計に入れない）。
            if (pe.th32ProcessID != 0)
            {
                all.push_back(Proc{pe.th32ProcessID, pe.th32ParentProcessID});
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);

    // 自 PID から子孫 PID を集める（BFS）。自身は既に合算済みなので子孫だけ足す。
    std::vector<DWORD> frontier;
    frontier.push_back(self_pid);
    std::vector<DWORD> descendants;
    descendants.reserve(64);
    while (!frontier.empty())
    {
        const DWORD parent = frontier.back();
        frontier.pop_back();
        for (const Proc& p : all)
        {
            if (p.ppid == parent && p.pid != parent && p.pid != self_pid)
            {
                // 既に集めた PID は二重に辿らない（循環・PID 再利用への保険）。
                bool seen = false;
                for (DWORD d : descendants)
                {
                    if (d == p.pid)
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    descendants.push_back(p.pid);
                    frontier.push_back(p.pid);
                }
            }
        }
    }

    // 各子孫の WorkingSetSize を合算する（読み取り専用権限で開く・開けないものはスキップ）。
    for (DWORD pid : descendants)
    {
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h == nullptr)
        {
            continue;
        }
        PROCESS_MEMORY_COUNTERS pmc;
        ZeroMemory(&pmc, sizeof(pmc));
        pmc.cb = sizeof(pmc);
        if (::GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
        {
            out.tree_ws_bytes += static_cast<std::uint64_t>(pmc.WorkingSetSize);
            out.process_count += 1;
        }
        ::CloseHandle(h);
    }

    return out;
}

void PerfLog::write_line(const std::string& line)
{
    if (!enabled_ || log_path_.empty())
    {
        return;
    }
    // 追記モード（UTF-8・wxFFile）。fopen は /WX で C4996 になるため使わない。失敗は握り潰す
    // （計測は観測のみ＝本体の挙動を妨げない・固まらない）。
    wxFFile f(wxString::FromUTF8(log_path_.c_str()), "ab");
    if (!f.IsOpened())
    {
        return;
    }
    const std::string out = line + "\r\n";
    f.Write(out.c_str(), out.size());
    f.Close();
}

} // namespace pika::app
