#include "core/diff/line_diff.h"

// dtl はヘッダオンリー（vcpkg が include/dtl/ に配置。CMake config は持たない）。
// Myers 差分の SES（最短編集スクリプト）だけを使う。dtl が using namespace std を持ち込む
// ヘッダのため、pika 側の名前空間に漏らさないよう本翻訳単位内に閉じて使う。
#include <dtl/dtl.hpp>

namespace pika::core::diff
{

std::vector<std::string> split_lines_lf(std::string_view content)
{
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < content.size(); ++i)
    {
        const char c = content[i];
        if (c == '\r' && i + 1 < content.size() && content[i + 1] == '\n')
        {
            continue; // CRLF の CR を落として直後の LF で行を確定する
        }
        if (c == '\n')
        {
            lines.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    // 末尾に改行が無ければ最後の行を確定する（"a\nb" は ["a","b"]、"a\n" は ["a"]）。
    if (!cur.empty())
    {
        lines.push_back(std::move(cur));
    }
    return lines;
}

std::vector<DiffLine> diff_lines(const std::vector<std::string>& old_lines,
                                 const std::vector<std::string>& new_lines)
{
    // dtl::Diff<elem, sequence>。elem=std::string（1 行）、sequence=std::vector<std::string>。
    dtl::Diff<std::string, std::vector<std::string>> d(old_lines, new_lines);
    d.compose();

    // SES（最短編集スクリプト）を unified 順に並べ替える。dtl の SES は原系列の出現順で
    // common / delete / add が混在するため、変更ブロックごとに「削除をまとめてから追加」を出す。
    const auto seq = d.getSes().getSequence();

    std::vector<DiffLine> out;
    out.reserve(seq.size());

    // 変更ブロックのバッファ。dtl の SES は変更ブロック内で add/delete を任意順で出すため、
    // ブロック内の削除・追加をいったん溜め、COMMON（または末尾）で「削除群→追加群」の
    // unified 順に確定する（要件8章「unified（インライン）表示」/ design.md 8章）。
    std::vector<DiffLine> pending_deletes;
    std::vector<DiffLine> pending_adds;
    std::size_t old_no = 0; // 1 始まりのベースライン行番号
    std::size_t new_no = 0; // 1 始まりの現在行番号

    auto flush_block = [&]() {
        for (auto& dl : pending_deletes)
        {
            out.push_back(std::move(dl));
        }
        for (auto& dl : pending_adds)
        {
            out.push_back(std::move(dl));
        }
        pending_deletes.clear();
        pending_adds.clear();
    };

    for (const auto& e : seq)
    {
        const std::string& text = e.first;
        const dtl::edit_t type = e.second.type;
        if (type == dtl::SES_DELETE)
        {
            ++old_no;
            DiffLine dl;
            dl.op = LineOp::Delete;
            dl.text = text;
            dl.old_line_no = old_no;
            pending_deletes.push_back(std::move(dl));
        }
        else if (type == dtl::SES_ADD)
        {
            ++new_no;
            DiffLine dl;
            dl.op = LineOp::Add;
            dl.text = text;
            dl.new_line_no = new_no;
            pending_adds.push_back(std::move(dl));
        }
        else // SES_COMMON
        {
            flush_block(); // 変更ブロックの終わり。削除群→追加群を確定してから context を出す
            ++old_no;
            ++new_no;
            DiffLine dl;
            dl.op = LineOp::Context;
            dl.text = text;
            dl.old_line_no = old_no;
            dl.new_line_no = new_no;
            out.push_back(std::move(dl));
        }
    }
    flush_block(); // 末尾が変更ブロックで終わる場合

    return out;
}

} // namespace pika::core::diff
