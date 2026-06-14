// core/render: 軽量 HTML トークナイザ。
// design.md 6章「文書由来HTMLはホワイトリスト方式サニタイズ」。HtmlSanitizer / HtmlInspector /
// RenderGuard の共通入力を作る。md4c が出す HTML と、ユーザー文書由来 HTML の双方を走査する。
//
// 完全な HTML5 パーサではなく、サニタイズ・検知に必要なだけの字句解析に絞る（YAGNI）。
// ただしセキュリティ判定の土台なので、コメント・属性のクォート種別（", ', なし）・自己終了・
// 生テキスト要素（script/style の中身を属性として誤認しない）を正しく区別する。
//
// 純ロジック（OS/WebView2 非依存）。gtest で決定論検証する。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pika::core::render
{

enum class TokenType
{
    Text,     // タグ外の地のテキスト
    StartTag, // <tag ...> または <tag .../>
    EndTag,   // </tag>
    Comment,  // <!-- ... -->
    Doctype,  // <!doctype ...> / <!... > / <? ... ?>（宣言・処理命令の総称）
};

struct Attribute
{
    std::string name;  // 小文字化済み
    std::string value; // 生値（エンティティ未解決。判定側で正規化する）
};

struct HtmlToken
{
    TokenType type = TokenType::Text;
    std::string name;             // StartTag/EndTag のタグ名（小文字化済み）
    std::string text;             // Text/Comment/Doctype の生テキスト
    std::vector<Attribute> attrs; // StartTag の属性
    bool self_closing = false;    // <tag/> なら true
};

// HTML 文字列をトークン列へ分解する。失敗しても例外を投げず、可能な範囲で字句化する
// （不正入力＝AI 出力の壊れた HTML を主対象とするため、頑健に倒す）。
std::vector<HtmlToken> tokenize_html(std::string_view html);

} // namespace pika::core::render
