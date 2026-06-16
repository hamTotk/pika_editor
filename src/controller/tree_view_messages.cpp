#include "controller/tree_view_messages.h"

namespace pika::controller
{

std::string_view state_mark_symbol(StateMark mark)
{
    // 記号は ui-design 5章の表に従う。色は付けない（呼び出し側がトークンを当てる）。
    switch (mark)
    {
    case StateMark::Diff:
        return "±"; // ± 差分あり
    case StateMark::New:
        return "◆"; // ◆ 新規ファイル
    case StateMark::Unsaved:
        return "●"; // ● 未保存
    case StateMark::DiffPropagated:
        return "±"; // ± 伝播（淡色は呼び出し側の配色で表現）
    case StateMark::Deleted:
        // 削除済みは取り消し線で表現し、記号は持たない（ui-design 5章）。
        return "";
    case StateMark::None:
        return "";
    }
    return "";
}

std::string_view state_mark_label(StateMark mark)
{
    switch (mark)
    {
    case StateMark::Diff:
        return "差分あり";
    case StateMark::New:
        return "新規";
    case StateMark::Unsaved:
        return "未保存";
    case StateMark::DiffPropagated:
        return "配下に差分あり";
    case StateMark::Deleted:
        return "削除済み";
    case StateMark::None:
        return "";
    }
    return "";
}

std::string_view icon_category_label(IconCategory icon)
{
    switch (icon)
    {
    case IconCategory::Folder:
        return "フォルダ";
    case IconCategory::Code:
        return "コード";
    case IconCategory::Data:
        return "データ";
    case IconCategory::Config:
        return "設定";
    case IconCategory::Script:
        return "スクリプト";
    case IconCategory::Image:
        return "画像";
    case IconCategory::Text:
        return "テキスト";
    case IconCategory::Unknown:
        return "ファイル";
    }
    return "ファイル";
}

} // namespace pika::controller
