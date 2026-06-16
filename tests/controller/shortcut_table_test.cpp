// controller/shortcut_table の検証（sprint6 must「ショートカット割当表」）。
// design.md 10章 J3 のフォーカス別ディスパッチを決定論テーブルとして観測する：
//   - Ctrl+Enter（差分/プレビュー）  → Confirm
//   - Ctrl+Shift+Enter（エディタ）   → Confirm（エディタの Ctrl+Enter は改行に譲る）
//   - Ctrl+Alt+Enter（フォーカス不問）→ ConfirmAll（一括確認）
//   - それ以外                       → None（既定動作を奪わない）
#include "controller/shortcut_table.h"

#include <gtest/gtest.h>

namespace
{

using pika::controller::dispatch_shortcut;
using pika::controller::FocusContext;
using pika::controller::KeyChord;
using pika::controller::ShortcutAction;

KeyChord ctrl_enter()
{
    KeyChord k;
    k.ctrl = true;
    k.enter = true;
    return k;
}

TEST(ShortcutTableTest, CtrlEnterConfirmsInDiffView)
{
    EXPECT_EQ(dispatch_shortcut(FocusContext::DiffView, ctrl_enter()), ShortcutAction::Confirm);
}

TEST(ShortcutTableTest, CtrlEnterConfirmsInPreview)
{
    EXPECT_EQ(dispatch_shortcut(FocusContext::Preview, ctrl_enter()), ShortcutAction::Confirm);
}

TEST(ShortcutTableTest, CtrlEnterDoesNotConfirmInEditor)
{
    // エディタでは Ctrl+Enter は改行挿入＝確認しない（既定動作を奪わない）。
    EXPECT_EQ(dispatch_shortcut(FocusContext::Editor, ctrl_enter()), ShortcutAction::None);
}

TEST(ShortcutTableTest, CtrlShiftEnterConfirmsInEditor)
{
    KeyChord k = ctrl_enter();
    k.shift = true;
    EXPECT_EQ(dispatch_shortcut(FocusContext::Editor, k), ShortcutAction::Confirm);
}

TEST(ShortcutTableTest, CtrlAltEnterConfirmsAllRegardlessOfFocus)
{
    KeyChord k = ctrl_enter();
    k.alt = true;
    // 一括確認はフォーカス非依存（どのペインでも発火する。J3・J6）。
    EXPECT_EQ(dispatch_shortcut(FocusContext::Editor, k), ShortcutAction::ConfirmAll);
    EXPECT_EQ(dispatch_shortcut(FocusContext::DiffView, k), ShortcutAction::ConfirmAll);
    EXPECT_EQ(dispatch_shortcut(FocusContext::Preview, k), ShortcutAction::ConfirmAll);
    EXPECT_EQ(dispatch_shortcut(FocusContext::Tree, k), ShortcutAction::ConfirmAll);
    EXPECT_EQ(dispatch_shortcut(FocusContext::Other, k), ShortcutAction::ConfirmAll);
}

TEST(ShortcutTableTest, TreeFocusGetsNoEnterConfirm)
{
    // ツリーフォーカスでの Ctrl+Enter は割当なし（誤確認を避ける）。
    EXPECT_EQ(dispatch_shortcut(FocusContext::Tree, ctrl_enter()), ShortcutAction::None);
}

TEST(ShortcutTableTest, PlainEnterIsUnassigned)
{
    // 修飾なしの Enter は割当なし（エディタ等の既定動作を奪わない）。
    KeyChord k;
    k.enter = true;
    EXPECT_EQ(dispatch_shortcut(FocusContext::DiffView, k), ShortcutAction::None);
    EXPECT_EQ(dispatch_shortcut(FocusContext::Editor, k), ShortcutAction::None);
}

TEST(ShortcutTableTest, CtrlWithoutEnterIsUnassigned)
{
    // Enter を伴わない Ctrl 入力は本表の対象外（None）。
    KeyChord k;
    k.ctrl = true;
    EXPECT_EQ(dispatch_shortcut(FocusContext::DiffView, k), ShortcutAction::None);
}

TEST(ShortcutTableTest, CtrlAltEnterTakesPriorityOverSingleConfirm)
{
    // Alt（一括）は Shift 有無や差分フォーカスより優先（一括の決め手は Alt）。
    KeyChord k = ctrl_enter();
    k.alt = true;
    k.shift = true;
    EXPECT_EQ(dispatch_shortcut(FocusContext::DiffView, k), ShortcutAction::ConfirmAll);
}

} // namespace
