#include "core/watcher/settle_reader.h"

namespace pika::core::watcher
{

void SettleReader::observe(const FileStat& st, TimeMs now)
{
    if (!seen_)
    {
        // 初回観測。基準値として記録する（まだ安定ではない＝確定不可）。
        seen_ = true;
        exists_ = st.exists;
        last_size_ = st.size;
        last_mtime_ns_ = st.mtime_ns;
        last_change_at_ = now;
        stable_count_ = 1;
        return;
    }

    const bool unchanged =
        (st.exists == exists_) && (st.size == last_size_) && (st.mtime_ns == last_mtime_ns_);
    if (unchanged)
    {
        ++stable_count_;
    }
    else
    {
        // size/mtime が変化＝まだ書き込み中。基準を更新し安定カウントと静穏起点をリセットする。
        exists_ = st.exists;
        last_size_ = st.size;
        last_mtime_ns_ = st.mtime_ns;
        last_change_at_ = now;
        stable_count_ = 1;
    }
}

bool SettleReader::ready_to_read(TimeMs now) const
{
    if (!seen_ || !exists_)
    {
        return false;
    }
    if (stable_count_ < stable_needed_)
    {
        return false;
    }
    // 最後の変化から静穏期間ぶん経過していること（中途内容で確定しない）。
    if (now < last_change_at_)
    {
        return false; // 時刻巻き戻りは安全側で未確定
    }
    return (now - last_change_at_) >= quiet_ms_;
}

void SettleReader::reset()
{
    seen_ = false;
    exists_ = false;
    last_size_ = 0;
    last_mtime_ns_ = 0;
    last_change_at_ = 0;
    stable_count_ = 0;
}

} // namespace pika::core::watcher
