#include "core/watcher/self_save_guard.h"

namespace pika::core::watcher
{

void SelfSaveGuard::register_save(const std::string& path, std::uint64_t hash_lf, TimeMs at)
{
    // 保存後ハッシュが取れないケース（0）は登録しない＝後続イベントは外部変更として扱う
    // （design.md 5.2「保存後ハッシュが取れないケースは外部変更として扱う」）。
    if (hash_lf == 0)
    {
        return;
    }
    tokens_[path].push_back(Token{hash_lf, at});
}

bool SelfSaveGuard::consume_if_self(const std::string& path, std::uint64_t disk_hash_lf, TimeMs now)
{
    (void)now; // 時刻は GC 専用。一致判定は時刻窓に依存しない（ハッシュ一致が主条件）。
    auto it = tokens_.find(path);
    if (it == tokens_.end())
    {
        return false;
    }
    auto& list = it->second;
    // ハッシュ一致するトークンを 1 件だけ消費（ワンショット）。複数保存が積まれていても
    // ディスク内容に一致する 1 件のみ消す（最古から走査し、最初の一致を消費）。
    for (auto tok = list.begin(); tok != list.end(); ++tok)
    {
        if (tok->hash_lf == disk_hash_lf)
        {
            list.erase(tok);
            if (list.empty())
            {
                tokens_.erase(it);
            }
            return true;
        }
    }
    return false;
}

void SelfSaveGuard::gc(TimeMs now)
{
    for (auto it = tokens_.begin(); it != tokens_.end();)
    {
        auto& list = it->second;
        for (auto tok = list.begin(); tok != list.end();)
        {
            // now が登録時刻 + window を超えたトークンを破棄する。
            // 単調増加時刻を前提とし、now < at の巻き戻りでは破棄しない（安全側）。
            if (now > tok->at && (now - tok->at) > window_ms_)
            {
                tok = list.erase(tok);
            }
            else
            {
                ++tok;
            }
        }
        if (list.empty())
        {
            it = tokens_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::size_t SelfSaveGuard::pending_count() const noexcept
{
    std::size_t n = 0;
    for (const auto& [path, list] : tokens_)
    {
        n += list.size();
    }
    return n;
}

} // namespace pika::core::watcher
