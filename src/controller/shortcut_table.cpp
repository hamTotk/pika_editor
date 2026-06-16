#include "controller/shortcut_table.h"

namespace pika::controller
{

ShortcutAction dispatch_shortcut(FocusContext focus, const KeyChord& chord)
{
    // 対象は Enter 系の確認ショートカットのみ（足さない）。Enter を伴わない入力は割当なし。
    if (!chord.enter || !chord.ctrl)
    {
        return ShortcutAction::None;
    }

    // 一括確認（Ctrl+Alt+Enter）はフォーカス非依存（どのペインでも発火する。design.md 10章
    // J3・J6）。 Shift の有無は問わない（Alt が一括の決め手）。最優先で判定する。
    if (chord.alt)
    {
        return ShortcutAction::ConfirmAll;
    }

    // ここから単一確認（Ctrl(+Shift)+Enter・Alt なし）。フォーカスで修飾キーの要否が変わる（J3）。
    switch (focus)
    {
    case FocusContext::DiffView:
    case FocusContext::Preview:
        // 差分/プレビューにフォーカスがあるときは Ctrl+Enter で確認済み（Shift
        // は不要・あっても可）。
        return ShortcutAction::Confirm;
    case FocusContext::Editor:
        // エディタでは Ctrl+Enter を改行挿入に譲り、確認済みは Ctrl+Shift+Enter で発火する。
        return chord.shift ? ShortcutAction::Confirm : ShortcutAction::None;
    case FocusContext::Tree:
    case FocusContext::Other:
        // ツリー等にフォーカスがあるときは Enter 確認を割り当てない（誤確認を避ける）。
        return ShortcutAction::None;
    }
    return ShortcutAction::None;
}

} // namespace pika::controller
