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
    case MsgId::MenuReview:
        return "レビュー(&R)";
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
    case MsgId::NotifyBlockedEncodingChoice:
        // 文字欠落を起こす前にユーザーへ救済選択を提示する（C3・要件5.2）。
        return "現在のエンコーディングで表現できない文字があります。"
               "UTF-8 で保存しますか？（キャンセルで保存を中断します）";
    case MsgId::SaveAsUtf8:
        return "UTF-8で保存";
    case MsgId::NotifyBlockedUnstashable:
        return "退避を取れないファイル（10MB以上・画像・機密）のため操作をブロックしました";
    case MsgId::NotifyStashFailed:
        return "退避に失敗したため上書き/確認を中止しました（内容は失われていません）";
    case MsgId::NotifySaveIoFailed:
        // 次の一手（再試行/別名保存/属性確認）を併記し『保存した』誤認を防ぐ（要件5.7・設計原則1）。
        return "保存に失敗しました。再試行/別名保存/書き込み権限の確認を（元の内容は無事です）";
    case MsgId::NotifyIndexSaveFailed:
        return "退避記録の保存に失敗しました（退避内容は保存済み・次の操作で再記録されます）";
    case MsgId::NotifyRollbackWriteFailed:
        return "巻き戻しの書き戻しに失敗しました。再試行を（巻き戻し前の内容は退避済みで安全です）";
    case MsgId::NotifyConfirmAllSkipped:
        // 件数つきの文言は notify_confirm_all_skipped(count) を使う（ここは件数なしの汎用文言）。
        return "一部のファイルは並行変化/退避失敗のため未確認のまま残りました";
    case MsgId::NotifyConfirmStaleRediff:
        return "外部変更を検出しました。差分を更新したので、内容を確認してから再度確認済みにして"
               "ください";
    case MsgId::NotifyConfirmNeedsSave:
        return "未保存の変更があります。保存してから確認済みにしてください";
    case MsgId::NotifyOpenInBrowser:
        return "既定のブラウザで開く";
    case MsgId::NotifySettingsInvalidValues:
        return "設定の一部が不正なため既定値を使用しました（settings.toml）";
    case MsgId::NotifySettingsParseFailed:
        return "settings.toml を読み取れないため、直前の有効な設定を維持しています";
    case MsgId::StatusLinkNotFound:
        return "リンク先が見つかりません";
    case MsgId::ConfirmClosePrompt:
        return "保存していない変更があります。閉じる前に保存しますか？";
    case MsgId::ConfirmExitPrompt:
        return "保存していない変更があります。保存して終了しますか？";
    case MsgId::ConfirmSave:
        return "保存";
    case MsgId::ConfirmSaveAll:
        return "すべて保存";
    case MsgId::ConfirmDiscard:
        return "破棄";
    case MsgId::ConfirmDiscardExit:
        return "保存しない";
    case MsgId::ConfirmCancel:
        return "キャンセル";
    case MsgId::OverflowNotices:
        // 件数は notify_overflow(count) を使う（ここは件数なしの汎用文言）。
        return "他にも通知があります";
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

std::string notify_confirm_all_skipped(std::size_t count)
{
    if (count == 0)
    {
        return std::string();
    }
    // 全件完了の誤認を防ぐため確認できなかった件数を即時提示する（要件8.3・design 5.4）。
    return std::to_string(count) + " 件は未確認のまま残りました（並行変化/退避失敗）";
}

std::string notify_overflow(std::size_t count)
{
    if (count == 0)
    {
        return std::string();
    }
    return "他 " + std::to_string(count) + " 件の通知";
}

std::string notification_kind_label(controller::NotificationKind kind)
{
    switch (kind)
    {
    case controller::NotificationKind::Conflict:
        return message(MsgId::NotifyConflict);
    case controller::NotificationKind::SettingsError:
        return "設定ファイルに不正な値があります（既定値で継続中）";
    case controller::NotificationKind::RemoteResource:
        return "プレビューが外部リソースを参照しています（既定で取得をブロック）";
    case controller::NotificationKind::JsDetected:
        return "文書に JavaScript が含まれています（プレビューで無効化）";
    case controller::NotificationKind::BigFile:
        return "巨大ファイルのため一部機能を縮退しています";
    }
    return std::string();
}

} // namespace pika::ui
