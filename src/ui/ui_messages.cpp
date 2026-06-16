#include "ui/ui_messages.h"

namespace pika::ui
{

std::string message(MsgId id)
{
    switch (id)
    {
    case MsgId::AppTitle:
        return "pika";
    case MsgId::MenuFile:
        return "ファイル(&F)";
    case MsgId::MenuOpenFolder:
        return "フォルダーを開く...\tCtrl+O";
    case MsgId::MenuClose:
        return "閉じる\tCtrl+W";
    case MsgId::MenuExit:
        return "終了\tAlt+F4";
    case MsgId::MenuView:
        return "表示(&V)";
    case MsgId::MenuRefresh:
        return "再読み込み";
    case MsgId::MenuHelp:
        return "ヘルプ(&H)";
    case MsgId::MenuAbout:
        return "pika について";
    case MsgId::TreePaneTitle:
        return "ファイル";
    case MsgId::StatusReady:
        return "準備完了";
    case MsgId::StatusNoFolder:
        return "フォルダーが開かれていません";
    case MsgId::StatusWatching:
        return "変更を監視中";
    case MsgId::StatusPolling:
        return "変更を定期確認中（監視不可）";
    case MsgId::StatusSyncing:
        return "再同期中...";
    case MsgId::EmptyNoFolder:
        return "フォルダーを開くと、ここにファイルが表示されます。";
    case MsgId::NotificationArea:
        return "通知";
    }
    return std::string();
}

std::string status_unread(std::size_t count)
{
    if (count == 0)
    {
        return "未読なし";
    }
    return "未読 " + std::to_string(count) + " 件";
}

} // namespace pika::ui
