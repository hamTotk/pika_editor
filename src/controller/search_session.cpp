#include "controller/search_session.h"

namespace pika::controller
{

using core::search::Match;
using core::search::SearchResult;

pika::util::Result<SearchResult> SearchSession::find_all(
    std::string_view text, std::string_view pattern, const core::search::SearchOptions& opts,
    const core::search::CancelTokenPtr& cancel) const
{
    return engine_.find_all(text, pattern, opts, cancel);
}

pika::util::Result<core::search::ReplaceResult> SearchSession::replace_all(
    std::string_view text, std::string_view pattern, std::string_view replacement,
    const core::search::SearchOptions& opts, const core::search::CancelTokenPtr& cancel) const
{
    return engine_.replace_all(text, pattern, replacement, opts, cancel);
}

NavTarget next_match(const SearchResult& result, std::size_t caret)
{
    NavTarget t;
    t.total = result.matches.size();
    if (result.matches.empty())
    {
        return t; // found=false
    }

    // caret 以降で最初に始まるヒット（find_all は begin 昇順）。同位置の選択を進めるため
    // begin>caret。
    for (std::size_t i = 0; i < result.matches.size(); ++i)
    {
        const Match& m = result.matches[i];
        if (m.begin > caret)
        {
            t.found = true;
            t.begin = m.begin;
            t.end = m.end;
            t.index = i;
            t.wrapped = false;
            return t;
        }
    }

    // 末尾まで無ければ先頭へ折り返す（要件5.4 の循環検索）。
    const Match& first = result.matches.front();
    t.found = true;
    t.begin = first.begin;
    t.end = first.end;
    t.index = 0;
    t.wrapped = true;
    return t;
}

NavTarget prev_match(const SearchResult& result, std::size_t caret)
{
    NavTarget t;
    t.total = result.matches.size();
    if (result.matches.empty())
    {
        return t; // found=false
    }

    // caret より前で最後に始まるヒット（begin 昇順を末尾から走査）。begin<caret で厳密に手前へ。
    for (std::size_t i = result.matches.size(); i-- > 0;)
    {
        const Match& m = result.matches[i];
        if (m.begin < caret)
        {
            t.found = true;
            t.begin = m.begin;
            t.end = m.end;
            t.index = i;
            t.wrapped = false;
            return t;
        }
    }

    // 先頭まで無ければ末尾へ折り返す（要件5.4 の循環検索）。
    const std::size_t last = result.matches.size() - 1;
    const Match& m = result.matches[last];
    t.found = true;
    t.begin = m.begin;
    t.end = m.end;
    t.index = last;
    t.wrapped = true;
    return t;
}

} // namespace pika::controller
