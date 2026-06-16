#include "controller/degrade_model.h"

namespace pika::controller
{

DegradeOutcome resolve_degrade(const DegradeInput& in)
{
    DegradeOutcome out;
    out.can_continue =
        true; // どの縮退でもアプリは継続する（クラッシュ・フリーズしない。要件12.1）。

    // 優先順位は DegradeKind
    // の並びどおり（読めない→無限展開→遅延読込→デコード爆発→監視縮退→保存誘導）。
    if (in.access_denied)
    {
        // アクセス権なし・排他ロック: リトライ後もダメならエラー表示へ縮退（内容は読めない）。
        out.kind = DegradeKind::AccessDenied;
        out.next_step = NextStep::RetryOrClose;
        out.blocks_content = true;
        return out;
    }
    if (in.symlink_loop)
    {
        // 循環参照: ツリーの無限展開を止める（その枝の展開を打ち切る）。
        out.kind = DegradeKind::SymlinkLoop;
        out.next_step = NextStep::None;
        out.blocks_content = true;
        return out;
    }
    if (in.cloud_placeholder)
    {
        // クラウドプレースホルダ:
        // 列挙では内容を読まず（ハイドレーション抑止）、開いたときだけ取得する。
        out.kind = DegradeKind::CloudPlaceholder;
        out.next_step = NextStep::OpenOnDemand;
        out.blocks_content =
            true; // フォルダ列挙時点では内容読込を行わない（全DL事故防止。要件12.1）。
        return out;
    }
    if (in.is_image && in.pixel_count > in.max_pixels)
    {
        // 巨大画像:
        // ヘッダ寸法で総ピクセル数を判定し、超過ならデコードせず外部アプリへ誘導（要件2.2/12.2）。
        out.kind = DegradeKind::ImageTooLarge;
        out.next_step = NextStep::OpenInDefaultApp;
        out.blocks_content = true; // デコードしない（固まらない）。
        return out;
    }
    if (in.network_drive)
    {
        // ネットワークドライブ/UNC: 監視不能ならポーリングへ縮退（機能は維持。内容は読める）。
        out.kind = DegradeKind::NetworkDrive;
        out.next_step = NextStep::PollingNotice;
        out.blocks_content = false;
        return out;
    }
    if (in.read_only)
    {
        // 読み取り専用属性: 開けるが、保存時に別名保存/属性解除へ誘導する（内容は読める）。
        out.kind = DegradeKind::ReadOnly;
        out.next_step = NextStep::SaveAsOrUnlock;
        out.blocks_content = false;
        return out;
    }

    // 縮退なし（通常表示）。
    out.kind = DegradeKind::None;
    out.next_step = NextStep::None;
    out.blocks_content = false;
    return out;
}

std::string_view degrade_kind_label(DegradeKind kind)
{
    switch (kind)
    {
    case DegradeKind::None:
        return "";
    case DegradeKind::AccessDenied:
        return "アクセス権がありません（他のアプリが使用中の可能性があります）";
    case DegradeKind::SymlinkLoop:
        return "リンクの循環を検出したため、この先の展開を中止しました";
    case DegradeKind::CloudPlaceholder:
        return "クラウド上のファイルです（開いたときにのみ取得します）";
    case DegradeKind::ImageTooLarge:
        return "画像が大きすぎるため表示できません";
    case DegradeKind::NetworkDrive:
        return "ネットワーク上のフォルダーです（定期確認に切り替えました）";
    case DegradeKind::ReadOnly:
        return "読み取り専用のファイルです";
    }
    return "";
}

std::string_view next_step_label(NextStep step)
{
    switch (step)
    {
    case NextStep::None:
        return "";
    case NextStep::OpenInDefaultApp:
        return "既定のアプリで開く";
    case NextStep::RetryOrClose:
        return "再試行する";
    case NextStep::SaveAsOrUnlock:
        return "名前を付けて保存する";
    case NextStep::PollingNotice:
        return "F5 で全体を再スキャンする";
    case NextStep::OpenOnDemand:
        return "クリックして取得する";
    }
    return "";
}

} // namespace pika::controller
