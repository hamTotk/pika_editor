// controller/tree_view_messages: TreeViewModel の列挙値 → 表示記号/日本語ラベルの単一定義。
// design.md 10章 K9「ユーザー向け文言は単一のメッセージ定義（ID→日本語文字列）経由で取得し、
// UI クラスに生文字列を直接書かない」。spec sprint1 should「生文字列を散らさない」。
//
// TreeViewModel（tree_view_model.h）は表示属性を列挙値（StateMark /
// IconCategory）で返し、表示文字列は
// 持たない。その列挙値から「状態記号（±/◆/取消線・伝播±淡）」と「アクセシブルネーム用の日本語ラベル」を
// ただ 1 箇所で写像する。これにより記号・文言がコード各所へ散らばらず、将来の多言語化の余地を残す。
// wx 非依存（pika_core に含め gtest で観測可能）。
#pragma once

#include "controller/tree_view_model.h"

#include <string_view>

namespace pika::controller
{

// 状態マークの表示記号（ui-design 5章）。色には依存しない（記号・形で弁別）。
// 削除済みは記号を持たない（取り消し線で表現）ため空文字を返す。None も空文字。
std::string_view state_mark_symbol(StateMark mark);

// 状態マークのアクセシブルネーム用ラベル（UIA/MSAA。ui-design 13章）。日本語。
std::string_view state_mark_label(StateMark mark);

// アイコンカテゴリの日本語ラベル（アクセシブルネーム・診断用）。
std::string_view icon_category_label(IconCategory icon);

} // namespace pika::controller
