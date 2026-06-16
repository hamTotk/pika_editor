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
    case MsgId::MenuSave:
        return "保存\tCtrl+S";
    case MsgId::MenuConfirm:
        return "確認済みにする\tCtrl+Enter";
    case MsgId::MenuConfirmAll:
        return "すべて確認済みにする\tCtrl+Alt+Enter";
    case MsgId::MenuRollback:
        return "確認済み時点に戻す";
    case MsgId::MenuView:
        return "表示(&V)";
    case MsgId::MenuRefresh:
        return "再読み込み";
    case MsgId::MenuModeSource:
        return "ソース";
    case MsgId::MenuModeSplit:
        return "分割";
    case MsgId::MenuModePreview:
        return "プレビュー";
    case MsgId::MenuToggleDiff:
        return "差分\tCtrl+Shift+D";
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
    case MsgId::StatusConfirmed:
        return "確認済みにしました";
    case MsgId::StatusSaved:
        return "保存しました";
    case MsgId::NotifyConflict:
        return "外部の変更を退避してから上書き保存しました";
    case MsgId::NotifyBlockedEncoding:
        return "現在のエンコーディングで表現できない文字があるため保存を中断しました";
    case MsgId::NotifyBlockedUnstashable:
        return "退避を取れないファイル（10MB以上・画像・機密）のため操作をブロックしました";
    case MsgId::NotifyStashFailed:
        return "退避に失敗したため上書き/確認を中止しました（内容は失われていません）";
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
