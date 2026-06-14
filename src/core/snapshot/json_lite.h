// core/snapshot: index.json / サイドカーメタ専用の最小 JSON。
// design.md 7章「index.json」「objects 側のサイドカーに併記」/ K2「version フィールド」。
//
// snapshot の台帳は固定スキーマ（数値・真偽・文字列・配列・オブジェクトのみ）で、外部入力ではなく
// pika 自身が書いた JSON のみを読む。固定スキーマに必要な最小の値モデルと読み書きだけを持つ
// （依存を増やさない。CLAUDE.md「軽い」。IPC=sprint7 とは用途が異なる）。
//
// 文字列は UTF-8 のまま扱い、`"` `\\` と制御文字のみエスケープする（ASCII 以外はそのまま通す）。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pika::core::snapshot::json
{

enum class Type
{
    Null,
    Bool,
    Int,
    String,
    Array,
    Object,
};

// 固定スキーマ用の値（数値は整数のみ。snapshot の台帳に小数は出ない）。
class Value
{
  public:
    Value() = default;
    static Value boolean(bool b);
    static Value integer(std::int64_t n);
    static Value str(std::string s);
    static Value array();
    static Value object();

    Type type() const noexcept { return type_; }
    bool is_object() const noexcept { return type_ == Type::Object; }
    bool is_array() const noexcept { return type_ == Type::Array; }

    bool as_bool(bool fallback = false) const noexcept;
    std::int64_t as_int(std::int64_t fallback = 0) const noexcept;
    const std::string& as_string() const noexcept { return string_; }

    std::vector<Value>& items() noexcept { return array_; }
    const std::vector<Value>& items() const noexcept { return array_; }

    // オブジェクトのメンバ設定・取得（挿入順を保つ）。
    void set(const std::string& key, Value v);
    const Value* get(const std::string& key) const noexcept;

    void push_back(Value v) { array_.push_back(std::move(v)); }

    std::string dump() const;

  private:
    Type type_ = Type::Null;
    bool bool_ = false;
    std::int64_t int_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> members_;
};

// JSON 文字列をパースする。成功時 true、失敗時 false（破損入力で例外を投げない）。
bool parse(std::string_view text, Value& out);

} // namespace pika::core::snapshot::json
