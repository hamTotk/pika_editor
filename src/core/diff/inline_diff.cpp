#include "core/diff/inline_diff.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace pika::core::diff
{

namespace
{

// 行内のトークン 1 個。原文上のバイト範囲 [begin,end) と、比較に使う実体（部分文字列ビュー）。
// 空白区切りトークン化では空白そのものもトークンにする（語の前後で境界を合わせ、強調を語単位に
// 寄せるため）。文字単位フォールバックでは 1 コードポイントが 1 トークンになる。
struct Token
{
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view view; // line.substr(begin, end-begin)。LCS の一致判定はこの内容で行う
};

bool is_ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\f' || c == '\v';
}

// UTF-8 先頭バイトから、そのコードポイントのバイト長を返す（不正系は 1 とみなして前進）。
std::size_t utf8_len(unsigned char lead)
{
    if (lead < 0x80)
    {
        return 1;
    }
    if ((lead >> 5) == 0x6)
    {
        return 2;
    }
    if ((lead >> 4) == 0xE)
    {
        return 3;
    }
    if ((lead >> 3) == 0x1E)
    {
        return 4;
    }
    return 1; // 継続バイト単独・不正先頭は 1 バイト前進（境界を割らない安全側）
}

// 空白区切りトークン化：連続する非空白を 1 語、連続する空白を 1 トークンにする。
std::vector<Token> tokenize_words(std::string_view line)
{
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < line.size())
    {
        const std::size_t start = i;
        const bool space = is_ascii_space(static_cast<unsigned char>(line[i]));
        while (i < line.size() && is_ascii_space(static_cast<unsigned char>(line[i])) == space)
        {
            ++i;
        }
        out.push_back(Token{start, i, line.substr(start, i - start)});
    }
    return out;
}

// 文字単位（UTF-8 コードポイント）トークン化。境界を割らない。
std::vector<Token> tokenize_chars(std::string_view line)
{
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < line.size())
    {
        std::size_t len = utf8_len(static_cast<unsigned char>(line[i]));
        if (i + len > line.size())
        {
            len = line.size() - i; // 末尾の不完全列は残り全部を 1 トークン（割らない）
        }
        out.push_back(Token{i, i + len, line.substr(i, len)});
        i += len;
    }
    return out;
}

// トークン列 a/b の LCS を取り、a 側で非共通＝削除、b 側で非共通＝追加の区間を out_a/out_b に返す。
// 隣接する非共通トークンは 1 区間にマージする（語/文字の連なりを 1 ハイライトにまとめる）。
void diff_tokens(const std::vector<Token>& a, const std::vector<Token>& b,
                 std::vector<InlineSpan>& out_a, std::vector<InlineSpan>& out_b)
{
    const std::size_t n = a.size();
    const std::size_t m = b.size();

    // 標準 LCS DP（O(n*m)）。呼び出し側が長行ガードで n*m の暴走を開始前に断つ前提。
    std::vector<std::vector<std::size_t>> dp(n + 1, std::vector<std::size_t>(m + 1, 0));
    for (std::size_t i = 1; i <= n; ++i)
    {
        for (std::size_t j = 1; j <= m; ++j)
        {
            if (a[i - 1].view == b[j - 1].view)
            {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            }
            else
            {
                dp[i][j] = dp[i - 1][j] >= dp[i][j - 1] ? dp[i - 1][j] : dp[i][j - 1];
            }
        }
    }

    // バックトラックして、各トークンが共通か非共通かを a/b それぞれに割り付ける。
    std::vector<bool> common_a(n, false);
    std::vector<bool> common_b(m, false);
    std::size_t i = n;
    std::size_t j = m;
    while (i > 0 && j > 0)
    {
        if (a[i - 1].view == b[j - 1].view)
        {
            common_a[i - 1] = true;
            common_b[j - 1] = true;
            --i;
            --j;
        }
        else if (dp[i - 1][j] >= dp[i][j - 1])
        {
            --i;
        }
        else
        {
            --j;
        }
    }

    // 非共通トークンを隣接マージして区間化する補助。空白だけの非共通トークンは強調しない
    // （語の前後の空白差で強調が散らからないようにする＝語単位の見やすさ。Context 寄せ）。
    auto build = [](const std::vector<Token>& toks, const std::vector<bool>& common,
                    std::vector<InlineSpan>& out) {
        std::size_t k = 0;
        while (k < toks.size())
        {
            if (common[k])
            {
                ++k;
                continue;
            }
            const std::size_t span_begin = toks[k].begin;
            std::size_t span_end = toks[k].end;
            ++k;
            while (k < toks.size() && !common[k])
            {
                span_end = toks[k].end;
                ++k;
            }
            out.push_back(InlineSpan{span_begin, span_end});
        }
    };

    build(a, common_a, out_a);
    build(b, common_b, out_b);
}

// 空白区切りで意味のある分割になるか。空白区切りトークンが 1 個以下（= 行全体が単一の語塊。
// 日本語など空白を含まない文）なら、語 LCS では行まるごと変更になり強調が機能しないため
// 文字単位へフォールバックする（design.md 8章「トークン境界が取れない場合」）。
bool words_are_separable(const std::vector<Token>& a, const std::vector<Token>& b)
{
    auto non_space_count = [](const std::vector<Token>& toks) {
        std::size_t c = 0;
        for (const auto& t : toks)
        {
            if (!t.view.empty() && !is_ascii_space(static_cast<unsigned char>(t.view.front())))
            {
                ++c;
            }
        }
        return c;
    };
    // 双方とも語が 2 個以上あって初めて「境界が取れる」とみなす。
    return non_space_count(a) >= 2 && non_space_count(b) >= 2;
}

// 行内 LCS の DP セル数上限。diff_tokens は (n+1)×(m+1) の std::size_t 行列を確保するため、
// トークン数が多い行（空白なしの巨大行＝CJK・URL・1 行 JSON 等）では n×m が GB 級になり OOM する。
// 別スレッド中断に頼らず、開始前にセル数で判定して暴走を断つ（design.md「固まらない」。差分本体の
// DiffLimits と同じ「開始前にサイズで弾く」思想）。約 4M セル ≒ 32MB（size_t 8B）で頭打ちにする。
constexpr std::size_t kMaxInlineDpCells = 4'000'000;

// n×m DP を張れるか（n*m のオーバーフローを避けて上限と比較する）。
bool within_dp_budget(std::size_t n, std::size_t m)
{
    if (n == 0 || m == 0)
    {
        return true; // 片側 0 は DP を張らずに済む（オーバーフローもしない）。
    }
    return n <= kMaxInlineDpCells / m;
}

bool is_utf8_continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

// n×m DP を張れない巨大行向けの軽量フォールバック（OOM 回避）。共通の先頭・末尾をバイト走査で
// 剥がし、中央の相違部分を各行 1 区間として返す（語/文字 LCS の近似）。剥がし位置は UTF-8 コード
// ポイント境界へスナップして文字を割らない（出力区間境界の不変条件を保つ）。O(行長) で完了する。
void trim_based_spans(std::string_view a, std::string_view b, std::vector<InlineSpan>& out_a,
                      std::vector<InlineSpan>& out_b)
{
    // 共通の先頭バイト数。等しい間だけ進め、コードポイント途中で切らないよう境界へ後退する。
    std::size_t prefix = 0;
    const std::size_t scan = std::min(a.size(), b.size());
    while (prefix < scan && a[prefix] == b[prefix])
    {
        ++prefix;
    }
    // 先頭領域はバイト一致なので a 側で境界を判定すれば b 側も同じ位置が境界になる。
    while (prefix > 0 && prefix < a.size() &&
           is_utf8_continuation(static_cast<unsigned char>(a[prefix])))
    {
        --prefix;
    }

    // 共通の末尾バイト数（先頭領域と重ならない範囲で）。
    std::size_t suffix = 0;
    const std::size_t suffix_scan = std::min(a.size() - prefix, b.size() - prefix);
    while (suffix < suffix_scan && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
    {
        ++suffix;
    }
    // 末尾領域の開始位置（a.size()-suffix）がコードポイント途中なら境界へ縮める。
    while (suffix > 0 && is_utf8_continuation(static_cast<unsigned char>(a[a.size() - suffix])))
    {
        --suffix;
    }

    // 中央の相違部分を各行 1 区間にする（空＝差分なしの側は区間を作らない）。
    if (prefix < a.size() - suffix)
    {
        out_a.push_back(InlineSpan{prefix, a.size() - suffix});
    }
    if (prefix < b.size() - suffix)
    {
        out_b.push_back(InlineSpan{prefix, b.size() - suffix});
    }
}

} // namespace

void compute_inline_spans(std::string_view old_line, std::string_view new_line,
                          std::vector<InlineSpan>& out_old, std::vector<InlineSpan>& out_new)
{
    out_old.clear();
    out_new.clear();

    std::vector<Token> wa = tokenize_words(old_line);
    std::vector<Token> wb = tokenize_words(new_line);

    if (words_are_separable(wa, wb))
    {
        if (within_dp_budget(wa.size(), wb.size()))
        {
            diff_tokens(wa, wb, out_old, out_new);
            return;
        }
        // 語数が多すぎて n×m DP を張れない（巨大行）。OOM を避けてトリム近似に倒す。
        trim_based_spans(old_line, new_line, out_old, out_new);
        return;
    }

    // フォールバック：文字単位（UTF-8 コードポイント）LCS。
    std::vector<Token> ca = tokenize_chars(old_line);
    std::vector<Token> cb = tokenize_chars(new_line);
    if (within_dp_budget(ca.size(), cb.size()))
    {
        diff_tokens(ca, cb, out_old, out_new);
        return;
    }
    // 文字数も多すぎて n×m DP を張れない（空白なしの巨大行）。OOM を避けてトリム近似に倒す。
    trim_based_spans(old_line, new_line, out_old, out_new);
}

} // namespace pika::core::diff
