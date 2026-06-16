#include "controller/view_state.h"

namespace pika::controller
{

namespace
{

// 縮退種別を「致命的に表示不能（Error）」へ畳むか判定する（ui-design 15章）。
// Error＝WebView2 不在・アクセス権なし・衝突（読み込み自体が不能）。
bool is_error_degrade(DegradeKind kind)
{
    return kind == DegradeKind::AccessDenied;
}

// 縮退種別を「機能縮退（Partial/degraded）」へ畳むか判定する（ui-design 15章）。
// Partial＝機能を縮退しつつ表示は継続（巨大画像はデコードせず誘導・クラウドは遅延・循環は枝打ち切り）。
// NetworkDrive（監視→ポーリング）・ReadOnly（保存時誘導）は本文表示自体は通常どおりのため、
// メイン表示の5状態は変えず通知バー側で扱う＝Partial へ畳まない（Ideal を妨げない）。
bool is_partial_degrade(DegradeKind kind)
{
    switch (kind)
    {
    case DegradeKind::SymlinkLoop:      // ツリー枝の展開を打ち切る（縮退表示）
    case DegradeKind::CloudPlaceholder: // 内容を遅延取得（縮退表示）
    case DegradeKind::ImageTooLarge:    // デコードせず外部誘導（縮退表示）
        return true;
    default:
        return false;
    }
}

} // namespace

ViewStateResult resolve_view_state(const ViewStateInput& in)
{
    ViewStateResult r;
    r.degrade_kind = in.degrade.kind;
    r.next_step = in.degrade.next_step;
    r.loaded_count = in.loaded_count;
    r.total_count = in.total_count;

    // (1) Error（最優先・上書きの強い）: 読み込み自体が不能（アクセス権なし・排他ロック）。
    //     機能を縮退してアプリは継続するが、メイン表示は Error 面＋次の一手。
    if (is_error_degrade(in.degrade.kind))
    {
        r.state = ViewState::Error;
        return r;
    }

    // (2) Partial（機能縮退）:
    // 差分/プレビュー自動オフ・ベースライン未取得・巨大画像・クラウド・循環。
    //     黙って切らず理由＋手動再有効化を通知バーで明示（ui-design 15章 Partial）。
    if (in.diff_auto_off || in.baseline_pending || is_partial_degrade(in.degrade.kind))
    {
        r.state = ViewState::Partial;
        return r;
    }

    // (3) Loading: 列挙中・ベースライン取得中（percent-done＋件数。UI は非ブロック）。
    if (in.loading)
    {
        r.state = ViewState::Loading;
        return r;
    }

    // (4) Empty: 表示項目がない（行き止まりにしない）。3分岐で文言を変える（ui-design
    // 15章・要件10章）。
    if (!in.has_visible_items)
    {
        r.state = ViewState::Empty;
        if (!in.folder_opened)
        {
            // フォルダ未オープン（初回）＝フォルダを開く CTA＋最近使った項目。
            r.empty_reason = EmptyReason::NoFolderOpened;
        }
        else if (in.is_search_mode)
        {
            // フォルダは開いているが検索 0 件＝検索条件を変える誘導。
            r.empty_reason = EmptyReason::SearchNoHits;
        }
        else if (in.all_consumed)
        {
            // 未読を全消化した後＝未読なしの達成状態。
            r.empty_reason = EmptyReason::AllConsumed;
        }
        else
        {
            // フォルダは開いているが項目が無い（空フォルダ等）＝NoFolderOpened に倒さず None。
            r.empty_reason = EmptyReason::None;
        }
        return r;
    }

    // (5) Ideal: 通常表示。
    r.state = ViewState::Ideal;
    r.empty_reason = EmptyReason::None;
    return r;
}

std::string_view empty_reason_label(EmptyReason reason)
{
    switch (reason)
    {
    case EmptyReason::None:
        return "";
    case EmptyReason::NoFolderOpened:
        return "フォルダーを開いて、AI の出力物を確認しましょう";
    case EmptyReason::SearchNoHits:
        return "一致する結果がありません（検索条件を変えてください）";
    case EmptyReason::AllConsumed:
        return "すべて確認済みです（未読はありません）";
    }
    return "";
}

} // namespace pika::controller
