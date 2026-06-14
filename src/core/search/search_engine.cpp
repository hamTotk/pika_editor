#include "core/search/search_engine.h"

#define PCRE2_CODE_UNIT_WIDTH 16
#include <pcre2.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pika::core::search
{

namespace
{

using util::ErrorCode;
using util::Result;

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

bool is_cancelled(const CancelTokenPtr& cancel)
{
    return cancel && cancel->is_cancelled();
}

// UTF-8 → UTF-16。各 UTF-16 コードユニット位置から元 UTF-8 バイト位置への対応表を作る。
// u16_to_u8[i] は u16[i] を生成した UTF-8 シーケンスの先頭バイト位置。末尾番兵として
// u16_to_u8[u16.size()] = u8.size() を入れる（終端＝全体一致 end の写し戻しに使う）。
// 不正バイトは U+FFFD（置換文字）に倒す（検索対象は妥当 UTF-8 前提だが堅牢側へ）。
struct Utf16Text
{
    std::u16string u16;
    std::vector<std::size_t> u16_to_u8; // 長さ = u16.size() + 1
};

void push_u16(Utf16Text& out, char16_t unit, std::size_t u8_pos)
{
    out.u16.push_back(unit);
    out.u16_to_u8.push_back(u8_pos);
}

Utf16Text to_utf16(std::string_view u8)
{
    Utf16Text out;
    out.u16.reserve(u8.size());
    out.u16_to_u8.reserve(u8.size() + 1);

    std::size_t i = 0;
    const std::size_t n = u8.size();
    while (i < n)
    {
        const std::size_t start = i;
        const unsigned char b0 = static_cast<unsigned char>(u8[i]);
        std::uint32_t cp = 0;
        std::size_t len = 1;
        if (b0 < 0x80)
        {
            cp = b0;
            len = 1;
        }
        else if ((b0 & 0xE0) == 0xC0 && i + 1 < n)
        {
            cp = b0 & 0x1F;
            len = 2;
        }
        else if ((b0 & 0xF0) == 0xE0 && i + 2 < n)
        {
            cp = b0 & 0x0F;
            len = 3;
        }
        else if ((b0 & 0xF8) == 0xF0 && i + 3 < n)
        {
            cp = b0 & 0x07;
            len = 4;
        }
        else
        {
            // 不正先頭バイトまたは末尾欠落。1 バイト消費して U+FFFD。
            push_u16(out, 0xFFFD, start);
            i += 1;
            continue;
        }

        bool valid = true;
        for (std::size_t k = 1; k < len; ++k)
        {
            const unsigned char bk = static_cast<unsigned char>(u8[i + k]);
            if ((bk & 0xC0) != 0x80)
            {
                valid = false;
                break;
            }
            cp = (cp << 6) | (bk & 0x3F);
        }
        if (!valid)
        {
            push_u16(out, 0xFFFD, start);
            i += 1;
            continue;
        }

        i += len;
        if (cp <= 0xFFFF)
        {
            push_u16(out, static_cast<char16_t>(cp), start);
        }
        else
        {
            cp -= 0x10000;
            const char16_t hi = static_cast<char16_t>(0xD800 + (cp >> 10));
            const char16_t lo = static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
            // サロゲートペアの両コードユニットは同じ UTF-8 先頭バイトに対応付ける。
            push_u16(out, hi, start);
            push_u16(out, lo, start);
        }
    }
    out.u16_to_u8.push_back(u8.size()); // 末尾番兵
    return out;
}

// PCRE2 のコンパイル済みパターン。RAII で必ず解放する。
class CompiledPattern
{
  public:
    CompiledPattern() = default;
    ~CompiledPattern()
    {
        if (mcontext_)
        {
            pcre2_match_context_free(mcontext_);
        }
        if (match_data_)
        {
            pcre2_match_data_free(match_data_);
        }
        if (code_)
        {
            pcre2_code_free(code_);
        }
    }
    CompiledPattern(const CompiledPattern&) = delete;
    CompiledPattern& operator=(const CompiledPattern&) = delete;

    pcre2_code* code = nullptr;
    pcre2_match_data* match_data = nullptr;
    // 計算量上限（match/depth limit）を設定したマッチコンテキスト。pcre2_match に渡して
    // 破滅的バックトラックを境界づける（自己 ReDoS 対策）。
    pcre2_match_context* mcontext = nullptr;

    // 上の public メンバは API 利用側の利便のために生ポインタを公開するが、
    // 解放責務はこの 3 つの内部ハンドルが持つ（own 経由でのみ所有させる）。
    void own(pcre2_code* c, pcre2_match_data* m, pcre2_match_context* mc)
    {
        code_ = c;
        match_data_ = m;
        mcontext_ = mc;
        code = c;
        match_data = m;
        mcontext = mc;
    }

  private:
    pcre2_code* code_ = nullptr;
    pcre2_match_data* match_data_ = nullptr;
    pcre2_match_context* mcontext_ = nullptr;
};

// リテラル文字列を正規表現メタ文字エスケープした UTF-16 パターンに変換する
// （opts.regex=false 用。PCRE2_LITERAL は単語境界ラップと両立しないため自前エスケープにする）。
std::u16string escape_literal(const std::u16string& s)
{
    std::u16string out;
    out.reserve(s.size() * 2);
    for (char16_t c : s)
    {
        switch (c)
        {
        case u'\\':
        case u'^':
        case u'$':
        case u'.':
        case u'[':
        case u']':
        case u'|':
        case u'(':
        case u')':
        case u'?':
        case u'*':
        case u'+':
        case u'{':
        case u'}':
            out.push_back(u'\\');
            break;
        default:
            break;
        }
        out.push_back(c);
    }
    return out;
}

// 検索パターン（UTF-16）を opts に応じて組み立てる。
// 単語単位は \b(?:...)\b で囲む（リテラル・正規表現の双方に適用）。
std::u16string build_pattern_u16(const Utf16Text& pattern, const SearchOptions& opts)
{
    std::u16string p = opts.regex ? pattern.u16 : escape_literal(pattern.u16);
    if (opts.whole_word)
    {
        std::u16string wrapped;
        wrapped.reserve(p.size() + 8);
        wrapped += u"\\b(?:";
        wrapped += p;
        wrapped += u")\\b";
        return wrapped;
    }
    return p;
}

// パターンをコンパイルする。失敗時は Result::err(InvalidArgument) を返す。
Result<bool> compile(CompiledPattern& cp, const std::u16string& pat, const SearchOptions& opts,
                     const SearchLimits& limits)
{
    // UCP で Unicode プロパティ（\p{...}・\w 等）を Unicode 定義に従わせる（要件5.4）。
    uint32_t options = PCRE2_UTF | PCRE2_UCP;
    if (!opts.case_sensitive)
    {
        options |= PCRE2_CASELESS;
    }

    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pat.c_str()), pat.size(), options,
                                     &errcode, &erroffset, nullptr);
    if (code == nullptr)
    {
        // エラーメッセージにパターン由来の文字列内容は載せない（診断ログ方針）。
        return Result<bool>::err(ErrorCode::InvalidArgument, "invalid regular expression");
    }
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(code, nullptr);
    if (md == nullptr)
    {
        pcre2_code_free(code);
        return Result<bool>::err(ErrorCode::Unknown, "failed to allocate match data");
    }
    // 計算量上限を載せたマッチコンテキスト。これを pcre2_match に渡すことで、ユーザー入力の
    // 正規表現による破滅的バックトラック（自己 ReDoS）を有限ステップで打ち切れる（sprint9 high）。
    pcre2_match_context* mc = pcre2_match_context_create(nullptr);
    if (mc == nullptr)
    {
        pcre2_match_data_free(md);
        pcre2_code_free(code);
        return Result<bool>::err(ErrorCode::Unknown, "failed to allocate match context");
    }
    pcre2_set_match_limit(mc, limits.match_limit);
    pcre2_set_depth_limit(mc, limits.depth_limit);
    cp.own(code, md, mc);
    return Result<bool>::ok(true);
}

// u16 オフセットを u8 バイトオフセットへ写し戻す（u16_to_u8 番兵込み）。
std::size_t map_back(const Utf16Text& t, std::size_t u16_off)
{
    if (u16_off >= t.u16_to_u8.size())
    {
        return t.u16_to_u8.empty() ? 0 : t.u16_to_u8.back();
    }
    return t.u16_to_u8[u16_off];
}

} // namespace

Result<SearchResult> SearchEngine::find_all(std::string_view text, std::string_view pattern,
                                            const SearchOptions& opts,
                                            const CancelTokenPtr& cancel) const
{
    SearchResult result;

    if (is_cancelled(cancel))
    {
        result.cancelled = true;
        return Result<SearchResult>::ok(std::move(result));
    }

    // 開始前のサイズガード（要件2.2 段階制。別スレッド中断に頼らない）。
    if (text.size() > limits_.max_total_bytes)
    {
        result.truncated = true;
        result.truncate_reason = TruncateReason::OversizeInput;
        return Result<SearchResult>::ok(std::move(result));
    }

    // 空パターンはヒット 0 件（無限空マッチを回避。UI 側で「未入力」と同義に扱う）。
    if (pattern.empty())
    {
        return Result<SearchResult>::ok(std::move(result));
    }

    const Utf16Text subject = to_utf16(text);
    const Utf16Text pat_text = to_utf16(pattern);
    const std::u16string built = build_pattern_u16(pat_text, opts);

    CompiledPattern cp;
    Result<bool> compiled = compile(cp, built, opts, limits_);
    if (compiled.is_err())
    {
        return Result<SearchResult>::err(compiled.error());
    }

    const PCRE2_SPTR subj = reinterpret_cast<PCRE2_SPTR>(subject.u16.c_str());
    const PCRE2_SIZE subj_len = subject.u16.size();

    PCRE2_SIZE start = 0;
    while (start <= subj_len)
    {
        if (is_cancelled(cancel))
        {
            result.cancelled = true;
            return Result<SearchResult>::ok(std::move(result));
        }

        const int rc = pcre2_match(cp.code, subj, subj_len, start, 0, cp.match_data, cp.mcontext);
        if (rc < 0)
        {
            if (rc == PCRE2_ERROR_NOMATCH)
            {
                // 正常な探索終了（この開始位置以降にヒットなし）。
                break;
            }
            if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
                rc == PCRE2_ERROR_HEAPLIMIT)
            {
                // 破滅的バックトラック等で計算量上限に到達（自己 ReDoS）。これ以上の列挙は危険な
                // ので安全に打ち切り、truncated=true で「全件ではない」と呼び出し側へ伝える
                // （ここまでに得たヒットは保持する）。
                result.truncated = true;
                result.truncate_reason = TruncateReason::ComplexityLimit;
                return Result<SearchResult>::ok(std::move(result));
            }
            // NOMATCH/limit 以外の負値（BADUTFOFFSET 等）。これを「ヒット無し」と握り潰すと以降を
            // 静かに取りこぼす。想定外なので truncated=true
            // で明示し、得たヒットだけ返す（防御的）。
            result.truncated = true;
            result.truncate_reason = TruncateReason::MatchError;
            return Result<SearchResult>::ok(std::move(result));
        }

        const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(cp.match_data);
        const PCRE2_SIZE m_begin = ov[0];
        const PCRE2_SIZE m_end = ov[1];

        Match m;
        m.begin = map_back(subject, m_begin);
        m.end = map_back(subject, m_end);

        const uint32_t group_count = pcre2_get_ovector_count(cp.match_data);
        m.groups.reserve(group_count);
        for (uint32_t g = 0; g < group_count; ++g)
        {
            const PCRE2_SIZE gb = ov[2 * g];
            const PCRE2_SIZE ge = ov[2 * g + 1];
            Match::Group grp;
            if (gb == PCRE2_UNSET || ge == PCRE2_UNSET)
            {
                grp.matched = false;
                grp.begin = kNpos;
                grp.end = kNpos;
            }
            else
            {
                grp.matched = true;
                grp.begin = map_back(subject, gb);
                grp.end = map_back(subject, ge);
            }
            m.groups.push_back(grp);
        }
        result.matches.push_back(std::move(m));

        if (result.matches.size() >= limits_.max_matches)
        {
            result.truncated = true;
            result.truncate_reason = TruncateReason::MaxMatches;
            return Result<SearchResult>::ok(std::move(result));
        }

        // 次の探索開始位置。空マッチ（m_end == m_begin）は無限ループを避けるため進める。
        if (m_end > m_begin)
        {
            start = m_end;
        }
        else
        {
            // ゼロ幅マッチの前進はコードポイント単位にする。m_end が上位サロゲートを指す場合に
            // +1 だけ進めると下位サロゲートの途中に着地し、PCRE2_UTF 有効・NO_UTF_CHECK 未指定の
            // pcre2_match が BADUTFOFFSET を返して以降を取りこぼす。BMP外文字（絵文字・CJK拡張）の
            // 直前のゼロ幅マッチでも正しく次へ進めるため、サロゲートペアなら 2 進める。
            const bool high_surrogate =
                m_end < subj_len && subject.u16[m_end] >= 0xD800 && subject.u16[m_end] <= 0xDBFF;
            const bool has_low = m_end + 1 < subj_len && subject.u16[m_end + 1] >= 0xDC00 &&
                                 subject.u16[m_end + 1] <= 0xDFFF;
            start = (high_surrogate && has_low) ? m_end + 2 : m_end + 1;
        }
    }

    return Result<SearchResult>::ok(std::move(result));
}

namespace
{

// replacement テンプレートを 1 マッチ分展開して out へ追記する（UTF-8）。
// $1 / ${12} / $0 / $$ と \1 / \0（後方参照）をサポート。未参加グループは空文字列。
// 範囲外の番号参照はリテラルとして残さず空に倒す（誤展開でデータ汚染しないため）。
void expand_replacement(std::string_view replacement, std::string_view subject, const Match& m,
                        std::string& out)
{
    const std::size_t n = replacement.size();
    std::size_t i = 0;

    auto append_group = [&](std::size_t idx) {
        if (idx < m.groups.size() && m.groups[idx].matched)
        {
            const auto& g = m.groups[idx];
            out.append(subject.substr(g.begin, g.end - g.begin));
        }
        // 未参加・範囲外は空（何も追記しない）。
    };

    while (i < n)
    {
        const char c = replacement[i];
        if (c == '$' && i + 1 < n)
        {
            const char d = replacement[i + 1];
            if (d == '$')
            {
                out.push_back('$');
                i += 2;
                continue;
            }
            if (d == '{')
            {
                std::size_t j = i + 2;
                std::size_t num = 0;
                bool any = false;
                while (j < n && replacement[j] >= '0' && replacement[j] <= '9')
                {
                    num = num * 10 + static_cast<std::size_t>(replacement[j] - '0');
                    any = true;
                    ++j;
                }
                if (any && j < n && replacement[j] == '}')
                {
                    append_group(num);
                    i = j + 1;
                    continue;
                }
                // 不正な ${...} はリテラルの '$' として残す。
                out.push_back('$');
                i += 1;
                continue;
            }
            if (d >= '0' && d <= '9')
            {
                std::size_t j = i + 1;
                std::size_t num = 0;
                while (j < n && replacement[j] >= '0' && replacement[j] <= '9')
                {
                    num = num * 10 + static_cast<std::size_t>(replacement[j] - '0');
                    ++j;
                }
                append_group(num);
                i = j;
                continue;
            }
        }
        if (c == '\\' && i + 1 < n)
        {
            const char d = replacement[i + 1];
            if (d >= '0' && d <= '9')
            {
                std::size_t j = i + 1;
                std::size_t num = 0;
                while (j < n && replacement[j] >= '0' && replacement[j] <= '9')
                {
                    num = num * 10 + static_cast<std::size_t>(replacement[j] - '0');
                    ++j;
                }
                append_group(num);
                i = j;
                continue;
            }
            // \\ → '\'、\n / \t などの一般エスケープは次文字をそのまま出す。
            if (d == '\\')
            {
                out.push_back('\\');
                i += 2;
                continue;
            }
            out.push_back(d);
            i += 2;
            continue;
        }
        out.push_back(c);
        ++i;
    }
}

} // namespace

Result<ReplaceResult> SearchEngine::replace_all(std::string_view text, std::string_view pattern,
                                                std::string_view replacement,
                                                const SearchOptions& opts,
                                                const CancelTokenPtr& cancel) const
{
    // 全ヒットを find_all で求め、ヒット間の原文を保ちながら置換テキストを組み立てる。
    Result<SearchResult> found = find_all(text, pattern, opts, cancel);
    if (found.is_err())
    {
        return Result<ReplaceResult>::err(found.error());
    }

    const SearchResult& sr = found.value();

    ReplaceResult rr;
    if (sr.truncated)
    {
        rr.truncated = true;
        rr.truncate_reason = sr.truncate_reason;
        return Result<ReplaceResult>::ok(std::move(rr));
    }
    if (sr.cancelled)
    {
        rr.cancelled = true;
        return Result<ReplaceResult>::ok(std::move(rr));
    }

    rr.text.reserve(text.size());
    std::size_t cursor = 0;
    for (const Match& m : sr.matches)
    {
        if (is_cancelled(cancel))
        {
            rr.cancelled = true;
            rr.text.clear();
            rr.replaced = 0;
            return Result<ReplaceResult>::ok(std::move(rr));
        }
        // 直前のヒット末尾から今回ヒット先頭までの原文をそのまま追記。
        rr.text.append(text.substr(cursor, m.begin - cursor));
        if (opts.regex)
        {
            expand_replacement(replacement, text, m, rr.text);
        }
        else
        {
            // リテラル置換はキャプチャ参照展開をしない（原文どおり）。
            rr.text.append(replacement);
        }
        cursor = m.end;
        ++rr.replaced;
    }
    rr.text.append(text.substr(cursor));

    return Result<ReplaceResult>::ok(std::move(rr));
}

} // namespace pika::core::search
