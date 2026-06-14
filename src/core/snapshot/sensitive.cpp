#include "core/snapshot/sensitive.h"

#include <algorithm>
#include <cctype>

namespace pika::core::snapshot
{

namespace
{

char lower_ascii(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// rel_path から最後の path 区切り（'/' または '\\'）以降のファイル名部分を取り出す。
std::string_view base_name(std::string_view rel_path)
{
    std::size_t pos = rel_path.find_last_of("/\\");
    if (pos == std::string_view::npos)
    {
        return rel_path;
    }
    return rel_path.substr(pos + 1);
}

// glob（`*` のみ対応・`?` や文字クラスは扱わない）を大小無視でファイル名に照合する。
// 機密パターン（`.env`・`*.key`・`*.pem`・`*secret*`）は `*` で十分なため最小実装に留める。
bool glob_match_ci(std::string_view pattern, std::string_view name)
{
    // 動的計画法。dp[j] = pattern[0..i) と name[0..j) が一致するか（i は外側ループで進める）。
    std::vector<char> prev(name.size() + 1, 0);
    std::vector<char> cur(name.size() + 1, 0);
    prev[0] = 1;
    for (std::size_t j = 1; j <= name.size(); ++j)
    {
        prev[j] = 0; // 空パターンは空文字列にしか一致しない
    }
    for (std::size_t i = 1; i <= pattern.size(); ++i)
    {
        const char pc = pattern[i - 1];
        cur[0] = (pc == '*') ? prev[0] : 0;
        for (std::size_t j = 1; j <= name.size(); ++j)
        {
            if (pc == '*')
            {
                // `*` は 0 文字以上に一致（左＝この `*` を使わない / 上＝1 文字消費して継続）。
                cur[j] = prev[j] || cur[j - 1];
            }
            else
            {
                cur[j] = prev[j - 1] && (lower_ascii(pc) == lower_ascii(name[j - 1]));
            }
        }
        std::swap(prev, cur);
    }
    return prev[name.size()] != 0;
}

} // namespace

std::vector<std::string> default_sensitive_patterns()
{
    // 要件9.1 の既定（.env・*.key・*.pem・*secret*）に、実運用で頻出する .env.local 等の派生も
    // 含める（`.env*`）。要件は「等」を許すため、データ最小化を強める側に倒す。
    return {".env*", "*.env", "*.key", "*.pem", "*secret*"};
}

bool is_sensitive(std::string_view rel_path, const std::vector<std::string>& patterns)
{
    const std::string_view name = base_name(rel_path);
    for (const auto& pat : patterns)
    {
        if (glob_match_ci(pat, name))
        {
            return true;
        }
    }
    return false;
}

bool is_sensitive_default(std::string_view rel_path)
{
    return is_sensitive(rel_path, default_sensitive_patterns());
}

} // namespace pika::core::snapshot
