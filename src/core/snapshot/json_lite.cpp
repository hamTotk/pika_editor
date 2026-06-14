#include "core/snapshot/json_lite.h"

#include <cctype>
#include <cstdio>

namespace pika::core::snapshot::json
{

Value Value::boolean(bool b)
{
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}

Value Value::integer(std::int64_t n)
{
    Value v;
    v.type_ = Type::Int;
    v.int_ = n;
    return v;
}

Value Value::str(std::string s)
{
    Value v;
    v.type_ = Type::String;
    v.string_ = std::move(s);
    return v;
}

Value Value::array()
{
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::object()
{
    Value v;
    v.type_ = Type::Object;
    return v;
}

bool Value::as_bool(bool fallback) const noexcept
{
    return type_ == Type::Bool ? bool_ : fallback;
}

std::int64_t Value::as_int(std::int64_t fallback) const noexcept
{
    return type_ == Type::Int ? int_ : fallback;
}

void Value::set(const std::string& key, Value v)
{
    for (auto& m : members_)
    {
        if (m.first == key)
        {
            m.second = std::move(v);
            return;
        }
    }
    members_.emplace_back(key, std::move(v));
}

const Value* Value::get(const std::string& key) const noexcept
{
    for (const auto& m : members_)
    {
        if (m.first == key)
        {
            return &m.second;
        }
    }
    return nullptr;
}

namespace
{

void escape_to(const std::string& s, std::string& out)
{
    out.push_back('"');
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xFF);
                out += buf;
            }
            else
            {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

} // namespace

// 各値型を JSON 文字列へ。Object のメンバ反復は private のため本メンバ関数で行い、入れ子は
// 再帰 dump() で組み立てる（非自明な分岐がないため free 関数に分けず 1 関数に集約する）。
std::string Value::dump() const
{
    std::string out;
    switch (type_)
    {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += bool_ ? "true" : "false";
        break;
    case Type::Int:
        out += std::to_string(int_);
        break;
    case Type::String:
        escape_to(string_, out);
        break;
    case Type::Array: {
        out.push_back('[');
        bool first = true;
        for (const auto& e : array_)
        {
            if (!first)
            {
                out.push_back(',');
            }
            first = false;
            out += e.dump();
        }
        out.push_back(']');
        break;
    }
    case Type::Object: {
        out.push_back('{');
        bool first = true;
        for (const auto& m : members_)
        {
            if (!first)
            {
                out.push_back(',');
            }
            first = false;
            escape_to(m.first, out);
            out.push_back(':');
            out += m.second.dump();
        }
        out.push_back('}');
        break;
    }
    }
    return out;
}

namespace
{

// ネストの深さ上限。index.json/サイドカーは外部編集されうるため、深いネスト入力での
// コールスタック枯渇を防ぐ（DoS 耐性）。台帳の実スキーマは entries→stash の 2 段で十分浅く、
// 余裕を持たせた値。超過時は false（破損扱い）に倒し、上位は退避走査の復元経路へ退避する。
constexpr int kMaxDepth = 64;

// 再帰下降パーサ。失敗時は false を返し out は不定（呼び出し側は false を破棄として扱う）。
class Parser
{
  public:
    explicit Parser(std::string_view t) : text_(t) {}

    bool parse(Value& out)
    {
        skip_ws();
        if (!parse_value(out))
        {
            return false;
        }
        skip_ws();
        return pos_ == text_.size();
    }

  private:
    void skip_ws()
    {
        while (pos_ < text_.size())
        {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                ++pos_;
            }
            else
            {
                break;
            }
        }
    }

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    bool parse_value(Value& out)
    {
        skip_ws();
        if (eof())
        {
            return false;
        }
        const char c = peek();
        if (c == '{' || c == '[')
        {
            // ネスト 1 段ぶん深さを進める。上限超過はスタック枯渇前に破損扱いで打ち切る。
            if (++depth_ > kMaxDepth)
            {
                return false;
            }
            const bool ok = (c == '{') ? parse_object(out) : parse_array(out);
            --depth_;
            return ok;
        }
        if (c == '"')
        {
            std::string s;
            if (!parse_string(s))
            {
                return false;
            }
            out = Value::str(std::move(s));
            return true;
        }
        if (c == 't' || c == 'f')
        {
            return parse_bool(out);
        }
        if (c == 'n')
        {
            return parse_null(out);
        }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            return parse_number(out);
        }
        return false;
    }

    bool parse_object(Value& out)
    {
        out = Value::object();
        ++pos_; // '{'
        skip_ws();
        if (!eof() && peek() == '}')
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            skip_ws();
            if (eof() || peek() != '"')
            {
                return false;
            }
            std::string key;
            if (!parse_string(key))
            {
                return false;
            }
            skip_ws();
            if (eof() || peek() != ':')
            {
                return false;
            }
            ++pos_;
            Value member;
            if (!parse_value(member))
            {
                return false;
            }
            out.set(key, std::move(member));
            skip_ws();
            if (eof())
            {
                return false;
            }
            if (peek() == ',')
            {
                ++pos_;
                continue;
            }
            if (peek() == '}')
            {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parse_array(Value& out)
    {
        out = Value::array();
        ++pos_; // '['
        skip_ws();
        if (!eof() && peek() == ']')
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            Value e;
            if (!parse_value(e))
            {
                return false;
            }
            out.push_back(std::move(e));
            skip_ws();
            if (eof())
            {
                return false;
            }
            if (peek() == ',')
            {
                ++pos_;
                continue;
            }
            if (peek() == ']')
            {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parse_string(std::string& out)
    {
        ++pos_; // 開き '"'
        out.clear();
        while (!eof())
        {
            const char c = text_[pos_++];
            if (c == '"')
            {
                return true;
            }
            if (c == '\\')
            {
                if (eof())
                {
                    return false;
                }
                const char e = text_[pos_++];
                switch (e)
                {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u': {
                    if (!parse_u_escape(out))
                    {
                        return false;
                    }
                    break;
                }
                default:
                    return false;
                }
            }
            else
            {
                out.push_back(c);
            }
        }
        return false;
    }

    // \uXXXX を UTF-8 へ変換する（サロゲートペアにも対応。台帳は ASCII 範囲以外を生で書くため
    // 通常は制御文字のみ \u だが、外部編集された台帳でも壊れず読めるよう正しく扱う）。
    bool parse_u_escape(std::string& out)
    {
        unsigned cp = 0;
        if (!read_hex4(cp))
        {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u')
            {
                return false;
            }
            pos_ += 2;
            unsigned lo = 0;
            if (!read_hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
            {
                return false;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        append_utf8(cp, out);
        return true;
    }

    bool read_hex4(unsigned& out)
    {
        if (pos_ + 4 > text_.size())
        {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9')
            {
                out |= static_cast<unsigned>(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                out |= static_cast<unsigned>(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F')
            {
                out |= static_cast<unsigned>(c - 'A' + 10);
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    static void append_utf8(unsigned cp, std::string& out)
    {
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_bool(Value& out)
    {
        if (text_.substr(pos_, 4) == "true")
        {
            pos_ += 4;
            out = Value::boolean(true);
            return true;
        }
        if (text_.substr(pos_, 5) == "false")
        {
            pos_ += 5;
            out = Value::boolean(false);
            return true;
        }
        return false;
    }

    bool parse_null(Value& out)
    {
        if (text_.substr(pos_, 4) == "null")
        {
            pos_ += 4;
            out = Value{};
            return true;
        }
        return false;
    }

    bool parse_number(Value& out)
    {
        const std::size_t start = pos_;
        if (!eof() && peek() == '-')
        {
            ++pos_;
        }
        bool has_digit = false;
        while (!eof() && peek() >= '0' && peek() <= '9')
        {
            ++pos_;
            has_digit = true;
        }
        // 台帳に小数・指数は出ない。万一含まれていれば整数として扱えないため失敗にする。
        if (!eof() && (peek() == '.' || peek() == 'e' || peek() == 'E'))
        {
            return false;
        }
        if (!has_digit)
        {
            return false;
        }
        const std::string num(text_.substr(start, pos_ - start));
        try
        {
            out = Value::integer(static_cast<std::int64_t>(std::stoll(num)));
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    int depth_ = 0; // 現在のネスト深さ（kMaxDepth でスタック枯渇を防ぐ）
};

} // namespace

bool parse(std::string_view text, Value& out)
{
    Parser p(text);
    return p.parse(out);
}

} // namespace pika::core::snapshot::json
