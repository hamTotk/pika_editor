#include "controller/tab_manager.h"

namespace pika::controller
{

std::size_t TabManager::index_of(const std::string& path) const
{
    for (std::size_t i = 0; i < tabs_.size(); ++i)
    {
        if (tabs_[i].path == path)
        {
            return i;
        }
    }
    return kNoActive;
}

const TabState* TabManager::at(std::size_t index) const
{
    if (index >= tabs_.size())
    {
        return nullptr;
    }
    return &tabs_[index];
}

std::size_t TabManager::open(const std::string& path, const std::string& title)
{
    // 同一 path が既にあれば重複オープンせずアクティブにする（タブの同一性キー＝絶対パス）。
    const std::size_t existing = index_of(path);
    if (existing != kNoActive)
    {
        active_ = existing;
        return existing;
    }

    TabState tab;
    tab.path = path;
    tab.title = title;
    tabs_.push_back(std::move(tab));
    active_ = tabs_.size() - 1;
    return active_;
}

void TabManager::close(std::size_t index)
{
    if (index >= tabs_.size())
    {
        return; // 範囲外は無視（クラッシュしない）
    }

    const bool closing_active = (index == active_);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));

    if (tabs_.empty())
    {
        active_ = kNoActive;
        return;
    }

    if (closing_active)
    {
        // アクティブを閉じた: 同位置（＝右隣が繰り上がる）を優先、末尾を閉じたら左隣へ。
        if (index < tabs_.size())
        {
            active_ = index;
        }
        else
        {
            active_ = tabs_.size() - 1;
        }
    }
    else if (active_ != kNoActive && index < active_)
    {
        // アクティブより左を閉じたらアクティブの index が 1 つ繰り上がる（同じタブを指し続ける）。
        --active_;
    }
}

void TabManager::activate(std::size_t index)
{
    if (index >= tabs_.size())
    {
        return; // 範囲外は無視
    }
    active_ = index;
}

void TabManager::mark_path_missing(const std::string& path)
{
    const std::size_t i = index_of(path);
    if (i == kNoActive)
    {
        return; // 該当タブなし
    }
    // 削除済み表示へ安全遷移（バッファは保持・閉じない。design.md 5.1 手順4・要件7.2）。
    // アクティブタブが消失してもアクティブは動かさない（未確認内容を保持したまま見せ続ける）。
    tabs_[i].path_missing = true;
}

void TabManager::set_unsaved(const std::string& path, bool unsaved)
{
    const std::size_t i = index_of(path);
    if (i == kNoActive)
    {
        return;
    }
    tabs_[i].unsaved = unsaved;
}

void TabManager::set_unread(const std::string& path, bool unread, bool has_baseline)
{
    const std::size_t i = index_of(path);
    if (i == kNoActive)
    {
        return;
    }
    tabs_[i].unread = unread;
    tabs_[i].has_baseline = has_baseline;
}

StateMark display_mark(const TabState& tab)
{
    // タブ状態を NodeStateInput へ写し、ツリーと同一の優先解決（削除済み ＞ 未保存 ＞ 差分あり）を
    // 使う（記号体系を一元化。tree_view_model::resolve_file_mark）。
    NodeStateInput s;
    s.deleted = tab.path_missing;
    s.unsaved = tab.unsaved;
    s.unread = tab.unread;
    s.has_baseline = tab.has_baseline;
    return resolve_file_mark(s);
}

// ---- フォルダ切替の状態機械（design.md 5.6） ----

FolderSwitchPhase FolderSwitch::begin(bool has_unsaved)
{
    // 未保存があれば確認段階から、無ければ即後始末へ（design.md 5.6 手順1→2）。
    phase_ = has_unsaved ? FolderSwitchPhase::ConfirmUnsaved : FolderSwitchPhase::TeardownCurrent;
    return phase_;
}

FolderSwitchPhase FolderSwitch::resolve_unsaved(UnsavedChoice choice)
{
    if (phase_ != FolderSwitchPhase::ConfirmUnsaved)
    {
        return phase_; // 確認段階以外では遷移しない（安全側）
    }
    switch (choice)
    {
    case UnsavedChoice::SaveAll:
    case UnsavedChoice::Discard:
        phase_ = FolderSwitchPhase::TeardownCurrent;
        break;
    case UnsavedChoice::Cancel:
        // キャンセルで切替を中止（現ワークスペース維持。design.md 5.6 手順1）。
        phase_ = FolderSwitchPhase::Cancelled;
        break;
    }
    return phase_;
}

FolderSwitchPhase FolderSwitch::teardown_done()
{
    if (phase_ != FolderSwitchPhase::TeardownCurrent)
    {
        return phase_;
    }
    phase_ = FolderSwitchPhase::EnumerateNew;
    return phase_;
}

} // namespace pika::controller
