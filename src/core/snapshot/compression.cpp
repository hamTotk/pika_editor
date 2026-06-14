#include "core/snapshot/compression.h"

#include <zstd.h>

namespace pika::core::snapshot
{

using pika::util::ErrorCode;
using pika::util::Result;

namespace
{

// 内容 object の圧縮レベル。差分・巻き戻しの起点で頻繁には書かないため、サイズ寄りの既定 3。
constexpr int kCompressLevel = 3;

// 展開後サイズの上限（解凍爆弾ガード）。内容 object・退避は編集可能テキスト（10MB 未満。
// kContentSizeLimit）が対象で、これを超えるファイルはそもそも内容を持たない。差し替えられた
// object が巨大サイズを宣言しても、out 確保前にこの上限で弾いて OOM を防ぐ。LF 正規化や
// 多少のメタ差を見込み 10MB の 4 倍を上限とする。
constexpr unsigned long long kMaxDecompressedSize = 40ull * 1024 * 1024;

} // namespace

Result<std::string> compress(std::string_view data)
{
    const std::size_t bound = ZSTD_compressBound(data.size());
    std::string out(bound, '\0');
    const std::size_t n =
        ZSTD_compress(out.data(), out.size(), data.data(), data.size(), kCompressLevel);
    if (ZSTD_isError(n))
    {
        return Result<std::string>::err(ErrorCode::Io, "zstd 圧縮に失敗しました");
    }
    out.resize(n);
    return Result<std::string>::ok(std::move(out));
}

Result<std::string> decompress(std::string_view compressed)
{
    const unsigned long long size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (size == ZSTD_CONTENTSIZE_ERROR)
    {
        return Result<std::string>::err(ErrorCode::Io, "zstd フレームが不正です");
    }
    if (size == ZSTD_CONTENTSIZE_UNKNOWN)
    {
        return Result<std::string>::err(ErrorCode::Io, "zstd 展開後サイズが不明です");
    }
    // 解凍爆弾ガード：out を確保する前に宣言サイズの上限を検査する（差し替え object 対策）。
    if (size > kMaxDecompressedSize)
    {
        return Result<std::string>::err(ErrorCode::Io, "zstd 展開後サイズが上限を超えています");
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    const std::size_t n =
        ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(n) || n != out.size())
    {
        return Result<std::string>::err(ErrorCode::Io, "zstd 展開に失敗しました");
    }
    return Result<std::string>::ok(std::move(out));
}

} // namespace pika::core::snapshot
